// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor S3 backup definitions
//
#pragma once

#include "abr_backup.h"

#include <aws/core/Aws.h>
#include <aws/core/utils/DateTime.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/ObjectLockMode.h>

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

    //
    // The Object Lock settings a probe found on a bucket. Reading them needs a
    // live bucket, but deciding what they mean does not, which is why the two
    // static members of S3BackupManager that judge them take them as data.
    //
    struct ObjectLockSettings
    {
        // Object Lock is switched on. On S3 that can only be done when the
        // bucket is created, so one without it stays mutable for good.
        bool m_enabled = false;

        // The default retention rule, which is what objects written without
        // retention headers of their own end up with. A rule may state its
        // period in calendar years; it is converted to days when probed, so
        // that one period can always be compared with another.
        ImmutabilityMode m_defaultMode = ImmutabilityMode::NONE;

        int m_days = 0;
    };

    //
    // The immutability a caller asked for.
    //
    struct ImmutabilityRequest
    {
        ImmutabilityMode m_mode = ImmutabilityMode::NONE;

        int m_days = 0;
    };

    //
    // The Object Lock retention written on the objects one backup is made of.
    // A backup that asked for no immutability carries none of its own and gets
    // whatever the bucket's default rule hands it.
    //
    struct ObjectLockRetention
    {
        ImmutabilityMode m_mode = ImmutabilityMode::NONE;

        // The moment the objects stop being retained. Fixed once, when the
        // backup is created, so that a long upload does not hand its last
        // object a later date than its first.
        Aws::Utils::DateTime m_retainUntil;
    };

    // The name of a mode as the tools spell it: none, governance or compliance.
    const char *immutabilityModeName(ImmutabilityMode mode);

    struct S3BackupManagerParams : BackupManagerParams
    {
        std::string     m_serverUrl;
        std::string     m_region;
        std::string     m_accessKey;
        std::string     m_secretKey;
        std::string     m_profile;
        ImmutabilityMode m_immutabilityMode = ImmutabilityMode::NONE;
        int     m_immutabilityDays = 0; // retention period new backups are held for, in days
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

        bool updateRetention(const std::string &path, const std::string &name,
                     ImmutabilityMode mode, int days) override;

        bool isExistingFile(const std::string &path) const;

        bool deleteFile(const std::string &path);

        Aws::S3::S3Client *getS3Client() {
            return m_s3_client;
        }

        // Turns the settings a probe found into the immutability that is actually
        // in effect for new backups. Static because it only reads its arguments:
        // checkImmutability() supplies them from a live bucket, a test states them
        // outright.
        static void resolveImmutability(const ObjectLockSettings &settings, BackupFolderInfo &effective);

        // Answers whether a bucket with these settings hands new backups at least
        // the mode and the period that were asked for. On a refusal `reason` is
        // left holding what falls short, as a phrase to report after the bucket
        // name.
        static bool honoursImmutabilityRequest(const BackupFolderInfo &effective,
                                               const ImmutabilityRequest &request,
                                               std::string &reason);

        // Turns the immutability a backup asks for into the retention its objects
        // are written with. Unlike the bucket's default rule, a retention put on
        // an object is the backup's own to choose, so only the mode has to be
        // within what the bucket supports: the period does not have to fit the
        // default one. On a refusal `reason` is left holding what falls short,
        // as a phrase to report after the bucket name.
        static bool resolveBackupRetention(const BackupFolderInfo &effective, const BackupInfo &info,
                                           ObjectLockRetention &retention, std::string &reason);

        // The same, for a mode and a period named on their own rather than read
        // off a backup. Creating a backup takes them from what it asked for,
        // updating one from what the caller now wants it held under, and both
        // have to land on the same retention for the same request.
        static bool resolveRetention(const BackupFolderInfo &effective, ImmutabilityMode mode, int days,
                                     ObjectLockRetention &retention, std::string &reason);

        // The retention left on an object, counted in the days the tools report.
        // Any part of a day still holds the object, so a remainder counts as a
        // whole day; a date already past holds nothing. Static because it only
        // reads its arguments: readBackupRetention() supplies them from a live
        // object, a test states them outright.
        static int retentionDaysLeft(const Aws::Utils::DateTime &retainUntil,
                                     const Aws::Utils::DateTime &now);

        // Stamps a retention onto an upload request. PutObject and
        // CreateMultipartUpload take it the same way, which is what the template
        // is for; an uploaded part carries no retention of its own, the upload
        // it belongs to does.
        template<typename RequestT>
        static void applyRetention(const ObjectLockRetention &retention, RequestT &request)
        {
            if (retention.m_mode != ImmutabilityMode::NONE) {
                request.SetObjectLockMode(retention.m_mode == ImmutabilityMode::COMPLIANCE
                        ? Aws::S3::Model::ObjectLockMode::COMPLIANCE
                        : Aws::S3::Model::ObjectLockMode::GOVERNANCE);
                request.SetObjectLockRetainUntilDate(retention.m_retainUntil);
            }
        }

    private:
        // Verify the configured bucket exists; create it when createBucketIfMissing
        // was requested. Returns false (with a diagnostic on std::cerr) when the
        // bucket is absent and cannot be provisioned.
        bool ensureBucket();

        // Probes the Object Lock configuration of the bucket and records in
        // m_params the mode and the retention period in days the target applies,
        // so getFolderInfo() reports the storage's own settings rather than what
        // the caller guessed. Returns false when an immutability mode or a
        // retention period was requested that the bucket does not deliver.
        bool checkImmutability();

        // Aborts every multipart upload still pending for the given key, so that
        // the parts a run interrupted mid upload left behind are released.
        bool abortPendingUploads(const std::string &key);

        // Reads back the Object Lock retention an object carries, as the mode it
        // is held in and the days it has left. Returns false when the target has
        // no retention to report -- an object written without one, or a target
        // that does not implement Object Lock at all -- which leaves `mode` NONE
        // and `days` 0, the same as an expired retention.
        bool readBackupRetention(const std::string &key, ImmutabilityMode &mode, int &days);

        // Writes a retention onto an object that already exists. Unlike
        // applyRetention(), which stamps one on the way in, this is what changes
        // the retention of a stored object.
        bool putObjectRetention(const std::string &key, const ObjectLockRetention &retention);

        Aws::S3::S3Client *m_s3_client;

        S3BackupManagerParams m_params;
    };
}
