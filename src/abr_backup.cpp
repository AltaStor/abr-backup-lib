// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ

#include "s3_backup.h"
#include "file_backup.h"
#include <iostream>

std::shared_ptr<Abr::IBackupManager> Abr::createBackupManager(const std::string &mode, const Abr::BackupManagerParams& params)
{
    std::shared_ptr<Abr::IBackupManager> pManager;

    if (mode == "file") {
        pManager = std::make_shared<Abr::FileBackupManager>();
        if (pManager == nullptr) {
            std::cerr << "Failed to create file backup manager!" << std::endl;
            return nullptr;
        }
    } else if (mode == "s3") {
        pManager = std::make_shared<Abr::S3BackupManager>();
        if (pManager == nullptr) {
            std::cerr << "Failed to create S3 backup manager!" << std::endl;
            return nullptr;
        }
    } else {
        std::cerr << "Invalid backup mode: '" << mode << "'!" << std::endl;
        return nullptr;
    }

    if (!pManager->initialize(params)) {
        std::cerr << "Failed to initialize backup manager!" << std::endl;
        pManager = nullptr;
    }

    return pManager;
}