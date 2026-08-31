// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor S3 backup definitions
//
#pragma once

#include "abr_backup.h"

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>

namespace Abr
{
    //
    // RAII wrapper for the AWS SDK global lifecycle.
    //
    // Aws::InitAPI/ShutdownAPI must bracket all AWS SDK usage and run while the
    // C runtime is still alive. Construct exactly one guard at the start of
    // main(); destroy it (after every S3 client/manager has been released) before
    // main() returns. Previously InitAPI/ShutdownAPI lived in S3BackupManager's
    // ctor/dtor, but that manager is owned by a global, so ShutdownAPI ran during
    // static destruction — after SDK statics were torn down — and segfaulted.
    //
    class AwsSdkGuard
    {
    public:
        AwsSdkGuard();
        ~AwsSdkGuard();

        AwsSdkGuard(const AwsSdkGuard &) = delete;
        AwsSdkGuard &operator=(const AwsSdkGuard &) = delete;
    };

    struct S3BackupManagerParams : BackupManagerParams
    {
        std::string     m_serverUrl;
        std::string     m_region;
        std::string     m_accessKey;
        std::string     m_secretKey;
        std::string     m_profile;
        bool    m_pathStyle = false;
        bool    m_verifyTls = true;
    };

    class S3BackupManager : public IBackupManager, public std::enable_shared_from_this<S3BackupManager>
    {
    public:
        S3BackupManager();

        S3BackupManager(const S3BackupManager &) = delete;
        S3BackupManager &operator=(const S3BackupManager &) = delete;
        S3BackupManager(S3BackupManager &&) = delete;
        S3BackupManager &operator=(S3BackupManager &&) = delete;
        
        ~S3BackupManager() override;

        bool initialize(const Abr::BackupManagerParams& params) override;

        bool listBackups(const std::string &path, std::vector<std::string> &backups) override;

        bool createBackup(const std::string &path, const std::string &name, const BackupInfo& info,
                     std::shared_ptr<IBackup> &pBackup, const std::string encryptionKey = "") override;

        bool openBackup(const std::string &path, const std::string &name,
                     std::shared_ptr<IRestore> &pRestore, const std::string decryptionKey = "") override;

        bool loadMetaData(const std::string &path, const std::string &name, std::ostream &metaData) override;

        bool saveMetaData(const std::string &path, const std::string &name, std::istream &metaData) override;

        bool deleteBackup(const std::string &path, const std::string &name) override;

        bool isExistingFile(const std::string &path) const;

        Aws::S3::S3Client *getS3Client() {
            return m_s3_client;
        }

    private:
        // Verify the configured bucket exists; create it when createBucketIfMissing
        // was requested. Returns false (with a diagnostic on std::cerr) when the
        // bucket is absent and cannot be provisioned.
        bool ensureBucket(const std::string &bucket);

        // Aborts every multipart upload still pending for the given key, so that
        // the parts a run interrupted mid upload left behind are released.
        bool abortPendingUploads(const std::string &key);

        Aws::S3::S3Client *m_s3_client;

        S3BackupManagerParams m_params;
    };
}
