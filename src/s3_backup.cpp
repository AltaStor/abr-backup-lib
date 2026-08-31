// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor S3 backup implementation
//

#include "s3_backup.h"
#include "abr_backup_format.h"
#include "backup_core.h"

#include <iostream>

#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>

#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/BucketLocationConstraint.h>
#include <aws/s3/model/GetObjectAttributesRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/ListMultipartUploadsRequest.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>

namespace Abr
{
    //
    // S3 backup implementation
    //
    class S3Backup : public Abr::BackupCore
    {
    public:
        S3Backup(std::shared_ptr<Abr::IBackupManager> pManager)
            : Abr::BackupCore(pManager)
        {
            m_minWriteSize = 0x500000; // 5 MB
            m_maxWriteSize = 0x800000; // 8 MB
        }

        ~S3Backup() override
        {
            // Removes what a backup that was never finalized has uploaded, for
            // example because the caller gave up on it without calling
            // finalizeBackup().
            discardPartialBackup();
        }

    protected:
        Handle createFile(const std::string &fullName, bool partialWrite) override;

        bool writeFile(Handle hFile, void *pDataBuf, uint32_t dataLength) override;

        bool closeFile(Handle hFile, bool bDelete = false) override;

        bool deleteFile(const std::string &fullName) override;
    };

    //
    // S3 restore from backup implementation
    //
    class S3Restore : public Abr::RestoreCore
    {
    public:
        S3Restore(std::shared_ptr<Abr::IBackupManager> pManager)
            : Abr::RestoreCore(pManager)
        {
        }

        ~S3Restore() override
        {
            if (m_hDataFile != nullptr) {
                close();
            }
        }

    protected:   
        Handle openFile(const std::string &fullName) override;

        bool readFile(Handle hFile, void *pDataBuf, uint64_t offset, uint32_t dataLength) override;

        bool closeFile(Handle hFile) override;

        bool deleteFile(const std::string &fullName) override;
    };
}

////////////

Abr::S3BackupManager::S3BackupManager()    : 
    m_s3_client(nullptr)
{
    // AWS SDK init/shutdown is owned by Abr::AwsSdkGuard at main() scope, not by
    // this manager — see the note on AwsSdkGuard in s3_backup.h.
}

Abr::S3BackupManager::~S3BackupManager()
{
    if (m_s3_client) {
        delete m_s3_client;
        m_s3_client = nullptr;
    }
}

//
// AWS SDK global lifecycle guard. The SDK options are kept in a single
// file-local instance so InitAPI and ShutdownAPI operate on the same object.
//
namespace
{
    Aws::SDKOptions g_awsSdkOptions;
}

Abr::AwsSdkGuard::AwsSdkGuard()
{
    // options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Trace;
    Aws::InitAPI(g_awsSdkOptions);
}

Abr::AwsSdkGuard::~AwsSdkGuard()
{
    Aws::ShutdownAPI(g_awsSdkOptions);
}

//
// Initialize the s3 client
//
// We can set AWS_PROFILE env variable before starting the program
// for example, export AWS_PROFILE=garage
//
bool Abr::S3BackupManager::initialize(const Abr::BackupManagerParams& params)
{
    if(params.m_folder.empty()) {
        std::cerr << "Bucket name is empty" << std::endl;
        return false;
    }
    
    m_params = reinterpret_cast<const S3BackupManagerParams &>(params);

    Aws::Client::ClientConfiguration config;

    if (!m_params.m_region.empty()) {
        config.region = m_params.m_region;
    }

    if (!m_params.m_serverUrl.empty()) {
        config.endpointOverride = m_params.m_serverUrl;
    }

    config.verifySSL = m_params.m_verifyTls;

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentialsProvider;

    if (!m_params.m_accessKey.empty() && !m_params.m_secretKey.empty()) {
        credentialsProvider = Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(
            "S3Provider",
            m_params.m_accessKey.c_str(),
            m_params.m_secretKey.c_str()
        );
    } else if (!m_params.m_profile.empty()) {
        credentialsProvider = Aws::MakeShared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(
            "S3Provider",
            m_params.m_profile.c_str()
        );
    } else {
        credentialsProvider = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>("S3Provider");
    }

    m_s3_client = new Aws::S3::S3Client(
        credentialsProvider,
        config,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        !m_params.m_pathStyle
    );
    if (!m_s3_client) {
        std::cerr << "Failed to create S3 client" << std::endl;
        return false;
    }

    m_folder = params.m_folder;

    if (!ensureBucket(m_folder)) {
        return false;
    }

    return true;
}

//
// Make sure the target bucket exists before any object operation is attempted.
//
// Without this, a missing bucket only surfaces deep inside createFile() as a
// confusing "CreateMultipartUpload failed: Bucket not found" followed by an
// "invalid file name" error. Here we probe with HeadBucket and, when the caller
// asked for it (backup / repository provisioning paths), create the bucket.
//
bool Abr::S3BackupManager::ensureBucket(const std::string &bucket)
{
    Aws::S3::Model::HeadBucketRequest headRequest;
    headRequest.SetBucket(bucket);

    auto headOutcome = m_s3_client->HeadBucket(headRequest);
    if (headOutcome.IsSuccess()) {
        return true;
    }

    if (!m_params.m_createFolderIfMissing) {
        std::cerr << "S3 bucket '" << bucket << "' is not accessible"
                  << (m_params.m_serverUrl.empty() ? "" : (" at endpoint " + m_params.m_serverUrl))
                  << ": " << headOutcome.GetError().GetMessage()
                  << ". Create the bucket or enable bucket auto-creation." << std::endl;
        return false;
    }

    Aws::S3::Model::CreateBucketRequest createRequest;
    createRequest.SetBucket(bucket);

    // Real AWS S3 requires a location constraint for every region other than
    // us-east-1. S3-compatible targets (Garage/MinIO/Ceph) reached through a
    // custom endpoint reject the constraint, so only send it for native AWS.
    if (m_params.m_serverUrl.empty() && !m_params.m_region.empty() && m_params.m_region != "us-east-1") {
        Aws::S3::Model::CreateBucketConfiguration bucketConfig;
        bucketConfig.SetLocationConstraint(
            Aws::S3::Model::BucketLocationConstraintMapper::GetBucketLocationConstraintForName(m_params.m_region));
        createRequest.SetCreateBucketConfiguration(bucketConfig);
    }

    auto createOutcome = m_s3_client->CreateBucket(createRequest);
    if (createOutcome.IsSuccess()) {
        std::cerr << "S3 bucket '" << bucket << "' did not exist; created it." << std::endl;
        return true;
    }

    // A concurrent backup may have created it between our Head and Create calls.
    const auto &err = createOutcome.GetError();
    if (err.GetErrorType() == Aws::S3::S3Errors::BUCKET_ALREADY_OWNED_BY_YOU ||
        err.GetErrorType() == Aws::S3::S3Errors::BUCKET_ALREADY_EXISTS) {
        return true;
    }

    std::cerr << "Failed to create S3 bucket '" << bucket << "'"
              << (m_params.m_serverUrl.empty() ? "" : (" at endpoint " + m_params.m_serverUrl))
              << ": " << err.GetMessage() << std::endl;
    return false;
}

bool Abr::S3BackupManager::isExistingFile(const std::string &path) const
{
    if (path.empty()) {
        std::cerr << "Backup name is empty" << std::endl;
        return false;
    }

    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(getFolder());
    request.SetKey(path);
    request.SetRange("bytes=0-0"); // Request the first byte to check if the object exists

    Aws::S3::Model::GetObjectOutcome outcome = m_s3_client->GetObject(request);

    return outcome.IsSuccess();
}

bool Abr::S3BackupManager::listBackups(const std::string &path, std::vector<std::string> &backups)
{
    Aws::String continuationToken;
    Aws::Vector<Aws::S3::Model::Object> allObjects;
    Aws::S3::Model::ListObjectsV2Request request;

    request.WithBucket(m_folder).WithPrefix(path);

    do {
        if (!continuationToken.empty()) {
            request.SetContinuationToken(continuationToken);
        }

        auto outcome = m_s3_client->ListObjectsV2(request);

        if (!outcome.IsSuccess()) {
            std::cerr << "Error: listObjects: " << outcome.GetError().GetMessage() << std::endl;
            return false;
        }
        
        Aws::Vector<Aws::S3::Model::Object> objects = outcome.GetResult().GetContents();

        allObjects.insert(allObjects.end(), objects.begin(), objects.end());
        continuationToken = outcome.GetResult().GetNextContinuationToken();

    } while (!continuationToken.empty());

    std::string ext = Abr::INDEX_FILE_EXT;
    std::string prefix = path;
    if (!prefix.empty() && prefix.back() != '/') {
        prefix.push_back('/');
    }

    for (const auto &object: allObjects) {
        const std::string key = object.GetKey();
        if (key.rfind(ext) != key.length() - ext.length()) {
            continue; // skip non index files
        }

        std::string name = key;
        if (!prefix.empty() && name.rfind(prefix, 0) == 0) {
            name.erase(0, prefix.length());
        }

        if (name.find('/') != std::string::npos) {
            continue;
        }

        name.erase(name.length() - ext.length());
        if (!name.empty()) {
            backups.push_back(name);
        }
    }

    return true;
}

bool Abr::S3BackupManager::createBackup(const std::string &path, const std::string &name,
    const BackupInfo& info, std::shared_ptr<IBackup> &pBackup, const std::string encryptionKey)
{
    std::shared_ptr<S3BackupManager> self = shared_from_this();
    auto backup = std::make_shared<S3Backup>(self);

    if (!backup->create(path, name, info, encryptionKey)) {
        std::cerr << "Failed to create backup " << name << " in " << path << std::endl;
        return false;
    }

    pBackup = backup;

    return true;
}

bool Abr::S3BackupManager::openBackup(const std::string &path, const std::string &name,
     std::shared_ptr<IRestore> &pRestore, const std::string decryptionKey)
{
    std::shared_ptr<S3BackupManager> self = shared_from_this();
    auto backup = std::make_shared<S3Restore>(self);

    if (!backup->open(path, name, decryptionKey)) {
        std::cerr << "Failed to open backup " << name << " in " << path << std::endl;
        return false;
    }

    pRestore = backup;
    
    return true;
}

bool Abr::S3BackupManager::loadMetaData(const std::string &path, const std::string &name, std::ostream &metaData)
{
    std::string fullPath = path + "/" + name + Abr::META_FILE_EXT;

    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(getFolder());
    request.SetKey(fullPath);
    
    request.SetResponseStreamFactory([&metaData]() {
        return Aws::New<Aws::IOStream>("GetObjectResponseStream", metaData.rdbuf());
    });

    auto outcome = m_s3_client->GetObject(request);

    if (!outcome.IsSuccess()) {
        std::cerr << "Error Getting object: " << outcome.GetError().GetMessage() << std::endl;
        return false;
    }

    return true;
}

bool Abr::S3BackupManager::saveMetaData(const std::string &path, const std::string &name, std::istream &metaData)
{
    std::string fullPath = path + "/" + name + Abr::META_FILE_EXT;

    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(getFolder());
    request.SetKey(fullPath);

    auto dataStream = Aws::MakeShared<Aws::IOStream>("PutObjectMetaDataStream", metaData.rdbuf());
    request.SetBody(dataStream);

    auto outcome = m_s3_client->PutObject(request);

    if (!outcome.IsSuccess()) {
        std::cerr << "Error: putObject: " << outcome.GetError().GetMessage() << std::endl;
        return false;
    }

    return true;
}

bool Abr::S3BackupManager::deleteBackup(const std::string &path, const std::string &name)
{
    auto deleteKey = [&](const std::string &key) -> bool {
        Aws::S3::Model::DeleteObjectRequest request;
        request.WithBucket(m_folder);
        request.WithKey(key);

        auto outcome = m_s3_client->DeleteObject(request);
        if (outcome.IsSuccess()) {
            return true;
        }

        auto err = outcome.GetError();
        if (err.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) {
            return true;
        }

        std::cerr << "Error: deleteObject: " << err.GetExceptionName() << ": " << err.GetMessage() << std::endl;
        return false;
    };

    std::string baseName = join_backup_path(path, name);

    bool dataDeleted = deleteKey(baseName + Abr::DATA_FILE_EXT);
    bool indexDeleted = deleteKey(baseName + Abr::INDEX_FILE_EXT);
    bool metaDataDeleted = deleteKey(baseName + Abr::META_FILE_EXT);

    // A run that died mid upload leaves a multipart upload of the data object
    // pending. Its parts are stored and billed while the object itself does not
    // exist yet, so deleting the keys alone would leave that partial data behind
    // with nothing left to reach it by.
    bool uploadsAborted = abortPendingUploads(baseName + Abr::DATA_FILE_EXT);

    return dataDeleted && indexDeleted && metaDataDeleted && uploadsAborted;
}

bool Abr::S3BackupManager::abortPendingUploads(const std::string &key)
{
    Aws::S3::Model::ListMultipartUploadsRequest request;
    request.WithBucket(m_folder).WithPrefix(key);

    bool ok = true;
    bool truncated = false;

    do {
        auto outcome = m_s3_client->ListMultipartUploads(request);
        if (!outcome.IsSuccess()) {
            // Not every S3 compatible server offers this listing. The objects of
            // the backup are deleted either way, so this is reported but does not
            // turn the deletion into a failure.
            std::cerr << "Warning: cannot check '" << key << "' for pending multipart uploads: "
                      << outcome.GetError().GetMessage() << std::endl;
            return true;
        }

        const auto &result = outcome.GetResult();

        for (const auto &upload : result.GetUploads()) {
            // The prefix also matches the uploads of longer names, only this key's
            // own uploads may be aborted.
            if (upload.GetKey() != Aws::String(key.c_str())) {
                continue;
            }

            Aws::S3::Model::AbortMultipartUploadRequest abortRequest;
            abortRequest.WithBucket(m_folder).WithKey(upload.GetKey()).WithUploadId(upload.GetUploadId());

            auto abortOutcome = m_s3_client->AbortMultipartUpload(abortRequest);
            if (!abortOutcome.IsSuccess()) {
                std::cerr << "Error aborting multipart upload of '" << upload.GetKey() << "': "
                          << abortOutcome.GetError().GetMessage() << std::endl;
                ok = false;
            }
        }

        truncated = result.GetIsTruncated();
        if (truncated) {
            request.SetKeyMarker(result.GetNextKeyMarker());
            request.SetUploadIdMarker(result.GetNextUploadIdMarker());
        }
    } while (truncated);

    return ok;
}

///////

class S3FileHandle 
{
public:
    S3FileHandle(const std::string &filename, bool partialWrite)
        : m_filename(filename)
        , m_isMultiPart(partialWrite)
    {}

    std::string m_filename;
    bool m_isMultiPart;

    Aws::String m_uploadID;
    
    int m_nextPartNumber = 1;

    Aws::Vector<Aws::S3::Model::CompletedPart> m_parts;
};

Handle Abr::S3Backup::createFile(const std::string &fullName, bool partialWrite)
{
    S3FileHandle *pFile = new S3FileHandle(fullName, partialWrite);
    if (!pFile) {
        std::cerr << "Failed to create file '" << fullName << "'!\n";
        return nullptr;
    }

    if (partialWrite) {
        auto pClient = static_cast<S3BackupManager*>(&*m_pManager)->getS3Client();

        Aws::S3::Model::CreateMultipartUploadRequest request;
        request.SetBucket(m_pManager->getFolder());
        request.SetKey(pFile->m_filename);

        auto outcome = pClient->CreateMultipartUpload(request);

        if (outcome.IsSuccess()) {
            pFile->m_uploadID = outcome.GetResult().GetUploadId();
        } else {
            std::cerr << "Error creating multipart upload: '" << pFile->m_filename << "': " << outcome.GetError().GetMessage() << std::endl;
            delete pFile;
            return nullptr;
        }
    }

    return pFile;
}

bool Abr::S3Backup::writeFile(Handle hFile, void *pDataBuf, uint32_t dataLength)
{
    S3FileHandle *pFile = static_cast<S3FileHandle*>(hFile);
    auto pClient = static_cast<S3BackupManager*>(&*m_pManager)->getS3Client();

    auto data = Aws::MakeShared<Aws::StringStream>("PutObjectBuffer", 
            std::stringstream::in | std::stringstream::out | std::stringstream::binary);
    data->write(reinterpret_cast<char*>(pDataBuf), dataLength);

    if (pFile->m_isMultiPart) {
        // It's a large multipart file
        Aws::S3::Model::UploadPartRequest request;
        request.SetBucket(m_pManager->getFolder());
        request.SetKey(pFile->m_filename);
        request.SetUploadId(pFile->m_uploadID);
        request.SetPartNumber(pFile->m_nextPartNumber++);
        request.SetBody(data);

        auto outcome = pClient->UploadPart(request);

        if (!outcome.IsSuccess()) {
            std::cerr << "Error: uploadPart: " << outcome.GetError().GetMessage() << std::endl;
            return false;
        }

        Aws::S3::Model::CompletedPart part;
        part.SetPartNumber(request.GetPartNumber());
        part.SetETag(outcome.GetResult().GetETag());
        pFile->m_parts.push_back(part);
    } else {
        // It's a single part write file
        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(m_pManager->getFolder());
        request.SetKey(pFile->m_filename);
        request.SetBody(data);

        auto outcome = pClient->PutObject(request);

        if (!outcome.IsSuccess()) {
            std::cerr << "Error: putObject: " << outcome.GetError().GetMessage() << std::endl;
            return false;
        }
    }

    return true;
}

bool Abr::S3Backup::closeFile(Handle hFile, bool bDelete)
{
    S3FileHandle *pFile = static_cast<S3FileHandle*>(hFile);

    if (!pFile) {
        return true;
    }

    auto pClient = static_cast<S3BackupManager*>(&*m_pManager)->getS3Client();

    if (bDelete) {
        if (pFile->m_isMultiPart) {
            Aws::S3::Model::AbortMultipartUploadRequest request;
            request.SetBucket(m_pManager->getFolder());
            request.SetKey(pFile->m_filename);
            request.SetUploadId(pFile->m_uploadID);

            auto outcome = pClient->AbortMultipartUpload(request);

            if (!outcome.IsSuccess()) {
                std::cerr << "Error aborting multipart upload: " << outcome.GetError().GetMessage() << std::endl;
                delete pFile;
                return false;
            }
        } else {
            Aws::S3::Model::DeleteObjectRequest request;
            request.WithBucket(m_pManager->getFolder());
            request.WithKey(pFile->m_filename);

            auto outcome = pClient->DeleteObject(request);

            if (!outcome.IsSuccess()) {
                auto err = outcome.GetError();
                if (err.GetResponseCode() != Aws::Http::HttpResponseCode::NOT_FOUND) {
                    std::cerr << "Error: deleteObject: " << err.GetExceptionName() << ": " << err.GetMessage() << std::endl;
                    delete pFile;
                    return false;
                }
            }
        }

        delete pFile;
        return true;
    }

    bool ok = true;
    if (pFile->m_isMultiPart) {
        Aws::S3::Model::CompletedMultipartUpload completedMultipartUpload;
        completedMultipartUpload.SetParts(pFile->m_parts);

        Aws::S3::Model::CompleteMultipartUploadRequest request;
        request.SetBucket(m_pManager->getFolder());
        request.SetKey(pFile->m_filename);
        request.SetUploadId(pFile->m_uploadID);
        request.SetMultipartUpload(completedMultipartUpload);

        auto outcome = pClient->CompleteMultipartUpload(request);

        if (!outcome.IsSuccess()) {
            // A failed completion leaves the object absent or truncated. Return
            // failure so the backup is reported as failed instead of silently
            // recording an unrestorable backup as successful.
            std::cerr << "Error completing multipart upload: " << outcome.GetError().GetMessage() << std::endl;
            ok = false;

            // The upload stays pending after a failed completion, with its parts
            // stored and billed. This handle holds the only reference to it, so it
            // has to be aborted here.
            Aws::S3::Model::AbortMultipartUploadRequest abortRequest;
            abortRequest.SetBucket(m_pManager->getFolder());
            abortRequest.SetKey(pFile->m_filename);
            abortRequest.SetUploadId(pFile->m_uploadID);

            auto abortOutcome = pClient->AbortMultipartUpload(abortRequest);

            if (!abortOutcome.IsSuccess()) {
                std::cerr << "Error aborting multipart upload: " << abortOutcome.GetError().GetMessage() << std::endl;
            }
        }
    }

    delete pFile;
    return ok;
}

bool Abr::S3Backup::deleteFile(const std::string &fullName)
{
    auto pClient = static_cast<S3BackupManager*>(&*m_pManager)->getS3Client();

    Aws::S3::Model::DeleteObjectRequest request;
    request.WithBucket(m_pManager->getFolder());
    request.WithKey(fullName);

    auto outcome = pClient->DeleteObject(request);

    if (!outcome.IsSuccess()) {
        auto err = outcome.GetError();
        if (err.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) {
            return true;
        }

        std::cerr << "Error: deleteObject: " << err.GetExceptionName() << ": " << err.GetMessage() << std::endl;
        return false;
    }

    return true;
}

Handle Abr::S3Restore::openFile(const std::string &fullName)
{
    S3FileHandle *pFile = new S3FileHandle(fullName, false);

    if (!pFile) {
        std::cerr << "Failed to open file '" << fullName << "'!\n";
        return nullptr;
    }

    return pFile;
}

bool Abr::S3Restore::readFile(Handle hFile, void *pDataBuf, uint64_t offset, uint32_t dataLength)
{
    // Nothing to read: a zero-length ranged GET would build an invalid
    // "bytes=offset-(offset-1)" request that some S3 servers answer with the
    // whole object, overflowing the caller's buffer.
    if (dataLength == 0) {
        return true;
    }

    S3FileHandle *pFile = static_cast<S3FileHandle*>(hFile);
    auto pClient = static_cast<S3BackupManager*>(&*m_pManager)->getS3Client();
    Aws::Utils::Stream::PreallocatedStreamBuf streambuf(reinterpret_cast<uint8_t*>(pDataBuf), dataLength);

    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(m_pManager->getFolder());
    request.SetKey(pFile->m_filename);
    std::string range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + dataLength - 1);
    request.SetRange(range);
    
    request.SetResponseStreamFactory([&streambuf]() {
        return Aws::New<Aws::IOStream>("", &streambuf);
    });

    auto outcome = pClient->GetObject(request);

    if (!outcome.IsSuccess()) {
        std::cerr << "Error Getting object: " << outcome.GetError().GetMessage() << std::endl;
        return false;
    }

    return true;
}

bool Abr::S3Restore::closeFile(Handle hFile)
{
    S3FileHandle *pFile = static_cast<S3FileHandle*>(hFile);

    delete pFile;

    return true;
}

bool Abr::S3Restore::deleteFile(const std::string &fullName)
{
    auto pClient = static_cast<S3BackupManager*>(&*m_pManager)->getS3Client();

    Aws::S3::Model::DeleteObjectRequest request;
    request.WithBucket(m_pManager->getFolder());
    request.WithKey(fullName);

    auto outcome = pClient->DeleteObject(request);

    if (!outcome.IsSuccess()) {
        auto err = outcome.GetError();
        std::cerr << "Error: deleteObject: " << err.GetExceptionName() << ": " << err.GetMessage() << std::endl;
        return false;
    }

    return true;
}
