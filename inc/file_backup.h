// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor file backup definitions
//
#pragma once

#include "abr_backup.h"

namespace Abr
{
    class FileBackupManager : public IBackupManager, public std::enable_shared_from_this<FileBackupManager>
    {
    public:
        FileBackupManager() = default;

        ~FileBackupManager() override;

        bool initialize(const Abr::BackupManagerParams& params) override;

        bool listBackups(const std::string &path, std::vector<std::string> &backups) override;

        bool createBackup(const std::string &path, const std::string &name, const BackupInfo& info,
                     std::shared_ptr<IBackup> &pBackup, const std::string encryptionKey = "") override;

        bool openBackup(const std::string &path, const std::string &name,
                     std::shared_ptr<IRestore> &pRestore, const std::string decryptionKey = "") override;

        bool loadMetaData(const std::string &path, const std::string &name, std::ostream &metaData) override;

        bool saveMetaData(const std::string &path, const std::string &name, std::istream &metaData) override;

        bool deleteBackup(const std::string &path, const std::string &name) override;
    };

}
