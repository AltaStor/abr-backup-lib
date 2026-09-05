// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor S3 backup implementation
//

#include "s3_backup.h"
#include "abr_backup_format.h"
#include "backup_core.h"

#include <chrono>
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
#include <aws/s3/model/GetObjectLockConfigurationRequest.h>
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
        S3Backup(std::shared_ptr<Abr::IBackupManager> pManager, const ObjectLockRetention &retention)
            : Abr::BackupCore(pManager)
            , m_retention(retention)
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

    private:
        // The Object Lock retention every object of this backup is written
        // with, as S3BackupManager::createBackup() resolved it. Empty when the
        // backup asked for no immutability of its own.
        ObjectLockRetention m_retention;
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

    if (!checkImmutability(m_folder)) {
        return false;
    }

    return true;
}

void Abr::S3BackupManager::getFolderInfo(const std::string &path, BackupFolderInfo &info)
{
    info.m_immutabilitySupported = m_params.m_immutabilitySupported;
    info.m_immutabilityCurrent = m_params.m_immutabilityCurrent;
    info.m_immutabilityDays = m_params.m_immutabilityDays;
}

namespace
{
    Abr::ImmutabilityMode toImmutabilityMode(Aws::S3::Model::ObjectLockRetentionMode mode)
    {
        switch (mode) {
        case Aws::S3::Model::ObjectLockRetentionMode::GOVERNANCE:
            return Abr::ImmutabilityMode::GOVERNANCE;
        case Aws::S3::Model::ObjectLockRetentionMode::COMPLIANCE:
            return Abr::ImmutabilityMode::COMPLIANCE;
        default:
            return Abr::ImmutabilityMode::NONE;
        }
    }

    // Retention periods only ever appear in days here, so a period reads the
    // same way wherever it is reported.
    std::string retentionPeriodName(int days)
    {
        return std::to_string(days) + " day(s)";
    }
}

const char *Abr::immutabilityModeName(ImmutabilityMode mode)
{
    switch (mode) {
    case ImmutabilityMode::GOVERNANCE:
        return "governance";
    case ImmutabilityMode::COMPLIANCE:
        return "compliance";
    default:
        return "none";
    }
}

//
// What new backups written to this bucket really get.
//
void Abr::S3BackupManager::resolveImmutability(const ObjectLockSettings &settings, BackupFolderInfo &effective)
{
    effective.m_immutabilitySupported = ImmutabilityMode::NONE;
    effective.m_immutabilityCurrent = ImmutabilityMode::NONE;
    effective.m_immutabilityDays = 0;

    if (!settings.m_enabled) {
        return;
    }

    // Object Lock is a bucket wide switch. Once on, an object may be
    // retained in either mode, and compliance is the stronger of the two.
    effective.m_immutabilitySupported = ImmutabilityMode::COMPLIANCE;

    // Backups are uploaded without per object retention headers, so what they
    // actually get is the default retention rule.
    effective.m_immutabilityCurrent = settings.m_defaultMode;
    effective.m_immutabilityDays = settings.m_days;

    // Neither half of the rule is worth anything without the other: a mode with
    // no period retains nothing, and a period with no mode is not enforced. A
    // bucket with Object Lock on but no usable rule keeps its backups
    // deletable, and has to say so.
    if (effective.m_immutabilityCurrent == ImmutabilityMode::NONE || effective.m_immutabilityDays <= 0) {
        effective.m_immutabilityCurrent = ImmutabilityMode::NONE;
        effective.m_immutabilityDays = 0;
    }
}

//
// Whether that is enough for what was asked for.
//
bool Abr::S3BackupManager::honoursImmutabilityRequest(const BackupFolderInfo &effective,
                                                      const ImmutabilityRequest &request,
                                                      std::string &reason)
{
    reason.clear();

    if (request.m_mode == ImmutabilityMode::NONE) {
        // Nothing was asked for, so whatever the bucket does is acceptable.
        return true;
    }

    if (effective.m_immutabilitySupported < request.m_mode) {
        reason = std::string("does not support ") + immutabilityModeName(request.m_mode) + " immutability";
        return false;
    }

    // Immutability being available is too high.
    if (effective.m_immutabilityCurrent > request.m_mode) {
        reason = effective.m_immutabilityCurrent == ImmutabilityMode::NONE
            ? std::string("applies no immutability to new backups")
            : std::string("applies ") + immutabilityModeName(effective.m_immutabilityCurrent)
                  + " immutability to new backups";

        reason += std::string(", while ") + immutabilityModeName(request.m_mode) + " was requested";
        return false;
    }

    if (effective.m_immutabilityDays > request.m_days) {
        reason = "retains new backups for " + retentionPeriodName(effective.m_immutabilityDays)
            + ", longer than the requested " + retentionPeriodName(request.m_days);
        return false;
    }

    return true;
}

//
// What one backup asked for, turned into the retention its objects carry.
//
bool Abr::S3BackupManager::resolveBackupRetention(const BackupFolderInfo &effective, const BackupInfo &info,
                                                  ObjectLockRetention &retention, std::string &reason)
{
    retention = ObjectLockRetention();
    reason.clear();

    if (info.m_immutabilityMode == ImmutabilityMode::NONE) {
        // The backup asked for nothing, so its objects are written without
        // retention of their own and fall to the bucket's default rule.
        return true;
    }

    if (effective.m_immutabilitySupported < info.m_immutabilityMode) {
        reason = std::string("does not support ") + immutabilityModeName(info.m_immutabilityMode)
            + " immutability";
        return false;
    }

    // A mode without a period retains nothing, so a backup that names the one
    // without the other has not said how long it wants to be kept.
    if (info.m_immutabilityDays <= 0) {
        reason = std::string("cannot hold a backup in ") + immutabilityModeName(info.m_immutabilityMode)
            + " mode for " + retentionPeriodName(info.m_immutabilityDays);
        return false;
    }

    retention.m_mode = info.m_immutabilityMode;
    retention.m_retainUntil = Aws::Utils::DateTime::Now()
        + std::chrono::milliseconds(static_cast<int64_t>(info.m_immutabilityDays) * 24 * 60 * 60 * 1000);

    return true;
}

//
// Find out whether the bucket can hold immutable backups at all, and in which
// mode and for how long new backups would be retained.
//
bool Abr::S3BackupManager::checkImmutability(const std::string &bucket)
{
    // What the caller asked for. The probe below replaces the fields with what
    // the bucket really applies, so the request is kept aside for validation.
    ImmutabilityRequest request;

    request.m_mode = m_params.m_immutabilityCurrent;
    request.m_days = m_params.m_immutabilityDays;

    ObjectLockSettings settings;

    Aws::S3::Model::GetObjectLockConfigurationRequest lockRequest;
    lockRequest.SetBucket(bucket);

    auto outcome = m_s3_client->GetObjectLockConfiguration(lockRequest);

    if (outcome.IsSuccess()) {
        const auto &lockConfig = outcome.GetResult().GetObjectLockConfiguration();
        const auto &retention = lockConfig.GetRule().GetDefaultRetention();

        settings.m_enabled = lockConfig.GetObjectLockEnabled() == Aws::S3::Model::ObjectLockEnabled::Enabled;
        settings.m_defaultMode = toImmutabilityMode(retention.GetMode());

        // A rule states its period in days or in calendar years, never in both,
        // and everything past this point counts in days. A year is taken as 365
        // of them, which is enough to order two periods; the exact retain until
        // date is still the bucket's to compute.
        settings.m_days = retention.GetYears() > 0 ? retention.GetYears() * 365 : retention.GetDays();
    } else {
        // Object Lock is optional: a bucket created without it answers
        // ObjectLockConfigurationNotFoundError, and S3 compatible targets that
        // do not implement the API at all answer NotImplemented or
        // MethodNotAllowed. Neither is a reason to fail a backup that does not
        // need immutability, so it is only reported when one was requested.
        if (request.m_mode != ImmutabilityMode::NONE) {
            const auto &err = outcome.GetError();
            std::cerr << "Failed to read the Object Lock configuration of S3 bucket '" << bucket << "'"
                      << (m_params.m_serverUrl.empty() ? "" : (" at endpoint " + m_params.m_serverUrl))
                      << ": " << err.GetExceptionName() << " " << err.GetMessage() << std::endl;
        }
    }

    BackupFolderInfo effective;

    resolveImmutability(settings, effective);

    m_params.m_immutabilitySupported = effective.m_immutabilitySupported;
    m_params.m_immutabilityCurrent = effective.m_immutabilityCurrent;
    m_params.m_immutabilityDays = effective.m_immutabilityDays;

    std::string reason;

    if (!honoursImmutabilityRequest(effective, request, reason)) {
        std::cerr << "S3 bucket '" << bucket << "' " << reason << "." << std::endl;

        // What can still be done about it depends on where it fell short: the
        // switch itself is out of reach after the bucket was created, the rule
        // written on top of it is not.
        std::cerr << (effective.m_immutabilitySupported == ImmutabilityMode::NONE
                          ? "Object Lock has to be enabled when the bucket is created."
                          : "Adjust the default retention rule of the bucket.")
                  << std::endl;
        return false;
    }

    return true;
}

//
// Make sure the target bucket exists before any object operation is attempted.
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
    // What the bucket was found to offer when the manager was initialized, so
    // that a backup asking for more than it can hold is refused before a single
    // object of it is uploaded.
    BackupFolderInfo effective;

    getFolderInfo(path, effective);

    ObjectLockRetention retention;
    std::string reason;

    if (!resolveBackupRetention(effective, info, retention, reason)) {
        std::cerr << "S3 bucket '" << getFolder() << "' " << reason << "." << std::endl;
        return false;
    }

    std::shared_ptr<S3BackupManager> self = shared_from_this();
    auto backup = std::make_shared<S3Backup>(self, retention);

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
        S3BackupManager::applyRetention(m_retention, request);

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
        S3BackupManager::applyRetention(m_retention, request);

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
    if (pFile->m_isMultiPart && pFile->m_parts.empty()) {
        // Nothing was ever written to this data file: the image held no data at
        // all, or a no-change incremental found nothing to store. S3 refuses to
        // complete an upload without a single part, so the upload is released
        // and the empty data file is written as a plain object instead. A backup
        // that stored nothing is a backup like any other, and every one of them
        // has a data file that opens.
        Aws::S3::Model::AbortMultipartUploadRequest abortRequest;
        abortRequest.SetBucket(m_pManager->getFolder());
        abortRequest.SetKey(pFile->m_filename);
        abortRequest.SetUploadId(pFile->m_uploadID);

        auto abortOutcome = pClient->AbortMultipartUpload(abortRequest);

        if (!abortOutcome.IsSuccess()) {
            // The upload holds no part, so nothing of it is stored or billed, and
            // whatever is left of it is released by the next deleteBackup(). The
            // object written below is what the backup is made of, so a failure to
            // tidy up here must not turn it into a failed backup.
            std::cerr << "Warning: the empty data upload of '" << pFile->m_filename
                      << "' stays pending until the backup is deleted: "
                      << abortOutcome.GetError().GetMessage() << std::endl;
        }

        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(m_pManager->getFolder());
        request.SetKey(pFile->m_filename);
        request.SetBody(Aws::MakeShared<Aws::StringStream>("PutObjectEmptyBuffer",
                std::stringstream::in | std::stringstream::out | std::stringstream::binary));
        S3BackupManager::applyRetention(m_retention, request);

        auto outcome = pClient->PutObject(request);

        if (!outcome.IsSuccess()) {
            std::cerr << "Error: putObject: " << outcome.GetError().GetMessage() << std::endl;
            ok = false;
        }
    } else if (pFile->m_isMultiPart) {
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
