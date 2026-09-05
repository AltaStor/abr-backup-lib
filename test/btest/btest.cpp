// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor backup test utility
//

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "s3_backup.h"
#include "file_backup.h"

namespace
{
    // Only the names of the enum are accepted, so that a typo is rejected
    // instead of quietly passing for "no immutability at all".
    bool ParseImmutabilityMode(const std::string &text, Abr::ImmutabilityMode &mode)
    {
        if (text == "none") {
            mode = Abr::ImmutabilityMode::NONE;
        } else if (text == "governance") {
            mode = Abr::ImmutabilityMode::GOVERNANCE;
        } else if (text == "compliance") {
            mode = Abr::ImmutabilityMode::COMPLIANCE;
        } else {
            return false;
        }

        return true;
    }

    // A retention period, counted in days like everywhere else in the library.
    bool ParseRetention(const std::string &text, int &days)
    {
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }

        try {
            days = std::stoi(text);
        } catch (const std::exception &) {
            return false;
        }

        return days > 0;
    }
}

int ListBackups(std::shared_ptr<Abr::IBackupManager> manager, const char *path)
{
    std::vector<std::string> backups;

    if (!manager->listBackups(path, backups))
    {
        std::cerr << "Failed to list backups in " << path << "!" << std::endl;
        return 1;
    }

    if (backups.empty()) {
        std::cerr << "No backups found in " << path << " /" << path << "!" << std::endl;
    } else {
        std::cout << backups.size() << " backup(s) found:" << std::endl;

        for (const auto &name: backups) {
            std::cout << "  " << name << std::endl;
        }
    }

    return 0;
}

int TestBackup(std::shared_ptr<Abr::IBackupManager> manager, const char *path, const char *name,
    Abr::ImmutabilityMode immutability, int immutabilityDays)
{
    Abr::BackupInfo info = {};
    std::shared_ptr<Abr::IBackup> pBackup;

    info.sectorSize = 512;
    info.phySectorSize = 512;
    info.sectors = 0x10000000;

    // What -i/-r ask for is written on the objects of this backup, on top of
    // whatever default rule the bucket applies to everything it holds.
    info.m_immutabilityMode = immutability;
    info.m_immutabilityDays = immutabilityDays;

    if (!manager->createBackup(path, name, info, pBackup)) {
        std::cerr << "Failed to create backup!\n";
        return 1;
    }

    // Half pattern, half zeroes: the first half is stored, the second one is
    // dropped by the empty block elimination, so both paths are exercised.
    char buf[0x2000] = {0};
    memset(buf, 'A', sizeof(buf) / 2);

    const struct {
        uint64_t offset;
        uint32_t length;
    } chunks[] = {
        { 0,         sizeof(buf) },
        { 0x10000,   sizeof(buf) },
        { 0x100000,  sizeof(buf) / 2 },
        { 0x1000000, sizeof(buf) },
    };

    for (const auto &chunk : chunks) {
        if (!pBackup->saveDataRange(buf, chunk.length, chunk.offset)) {
            std::cerr << "Failed to save data range at offset " << chunk.offset << "!\n";
            // Drops what has been written so far instead of leaving a partial
            // backup behind in the repository.
            pBackup->finalizeBackup(true);
            return 1;
        }
    }

    if (!pBackup->finalizeBackup()) {
        std::cerr << "Failed to finalize backup!\n";
        return 1;
    }

    std::cout << "Backup '" << name << "' created.\n";

    return 0;
}

int TestRestore(std::shared_ptr<Abr::IBackupManager> manager, const char *path, const char *name)
{
    std::shared_ptr<Abr::IRestore> pRestore;

    if (!manager->openBackup(path, name, pRestore)) {
        std::cerr << "Failed to open backup!\n";
        return 1;
    }

    char buf[0x1000] = {0};

    auto &info = pRestore->getInfo();

    std::cout << "Backup sectors: " << info.sectors << ", sectorSize " << info.sectorSize << ".\n";

    std::vector<Abr::Range> ranges;

    if (!pRestore->getDataRanges(ranges)) {
        std::cerr << "Failed to get ranges!\n";
        return 1;
    }

    std::cout << "Load ranges:\n";

    bool succeeded = true;
    uint32_t i = 0;
    for (const auto &range : ranges) {
        if (pRestore->loadDataRange(i, buf, std::min(sizeof(buf), (size_t)range.length), 0)) {
            std::cout << ++i << ") offset: " << range.offset / info.sectorSize << ", length " << range.length / info.sectorSize << "\n";
        } else {
            std::cerr << "Failed to load range " << ++i << ") offset: " << range.offset / info.sectorSize << ", length "
                    << range.length / info.sectorSize << "!\n";
            succeeded = false;
            break;
        }
    }

    pRestore->close();

    return succeeded ? 0 : 1;
}

int TestRemove(std::shared_ptr<Abr::IBackupManager> manager, const char *path, const char *name)
{
    std::shared_ptr<Abr::IRestore> pRestore;

    if (!manager->openBackup(path, name, pRestore)) {
        std::cerr << "Failed to open backup!\n";
        return 1;
    }

    if (!pRestore->remove()) {
        std::cerr << "Failed to remove backup!\n";
        return 1;
    }

    std::cout << "Backup removed.\n";

    return 0;
}

int TestEncryption(std::shared_ptr<Abr::IBackupManager> manager, const char *path, const char *name)
{
    Abr::BackupInfo sinfo = {};
    std::shared_ptr<Abr::IBackup> pBackup;
    std::string encryptionKey = "0123456789abcdef0123456789abcdef"; // 32 bytes key for AES-256-GCM

    sinfo.sectorSize = 512;
    sinfo.phySectorSize = 512;
    sinfo.sectors = 0x10000000;
    sinfo.encryption = 1;

    if (!manager->createBackup(path, name, sinfo, pBackup, encryptionKey)) {
        std::cerr << "Failed to create backup!\n";
        return 1;
    }

    char buf[0x2000] = "1234567890";
    char buf2[0x2000] = {0};

    if (!pBackup->saveDataRange(buf, sizeof(buf), 0x1234)) {
        std::cerr << "Failed to save data range!\n";
        pBackup->finalizeBackup(true);
        return 1;
    }

    if (!pBackup->finalizeBackup()) {
        std::cerr << "Failed to finalize backup!\n";
        return 1;
    }

    // Try to restore

    std::shared_ptr<Abr::IRestore> pRestore;

    if (!manager->openBackup(path, name, pRestore, encryptionKey)) {
        std::cerr << "Failed to open backup!\n";
        return 1;
    }

    auto &info = pRestore->getInfo();

    std::cout << "Backup sectors: " << info.sectors << ", sectorSize " << info.sectorSize << ".\n";

    std::vector<Abr::Range> ranges;

    if (!pRestore->getDataRanges(ranges)) {
        std::cerr << "Failed to get ranges!\n";
        return 1;
    }

    // The single range written above has to come back, otherwise nothing was
    // compared and the round trip cannot be called a success.
    if (ranges.empty()) {
        std::cerr << "No data ranges in the encrypted backup!\n";
        pRestore->close();
        return 1;
    }

    std::cout << "Load ranges:\n";

    bool succeeded = true;
    uint32_t i = 0;
    for (const auto &range : ranges) {
        if (pRestore->loadDataRange(i, buf2, std::min(sizeof(buf2), (size_t)range.length), 0)) {
            if (memcmp(buf, buf2, std::min(sizeof(buf), (size_t)range.length)) == 0) {
                std::cout << "Range " << ++i << " data matches original.\n";
            } else {
                std::cerr << "Range " << ++i << " data does NOT match original!\n";
                succeeded = false;
                break;
            }
        } else {
            std::cerr << "Failed to load range " << ++i << ") offset: " << range.offset / info.sectorSize << ", length "
                    << range.length / info.sectorSize << "!\n";
            succeeded = false;
            break;
        }
    }

    pRestore->close();

    return succeeded ? 0 : 1;
}

//
// Reports what the storage really does with the backups it is given: whether it
// can hold them immutably at all, and in which mode and for how long new ones
// would be retained. Combined with --immutable/--retention it also covers the
// other half of the check, since a request the target cannot honour makes the
// manager fail to initialize before this ever runs.
//
int TestImmutability(std::shared_ptr<Abr::IBackupManager> manager, const char *path)
{
    Abr::BackupFolderInfo info;

    manager->getFolderInfo(path, info);

    std::cout << "Immutability of '" << manager->getFolder() << "/" << path << "':\n"
              << "  supported: " << Abr::immutabilityModeName(info.m_immutabilitySupported) << "\n"
              << "  current:   " << Abr::immutabilityModeName(info.m_immutabilityCurrent) << "\n";

    if (info.m_immutabilityCurrent == Abr::ImmutabilityMode::NONE) {
        std::cout << "  retention: none, new backups stay deletable\n";
    } else {
        std::cout << "  retention: " << info.m_immutabilityDays << " day(s)\n";
    }

    return 0;
}

int PrintUsage()
{
    std::cout << R"(
Usage:
    btest [options] <bucket/dir> <command> [parameters]
      Options:
        -f, --file     Use a local folder instead of an S3 bucket
        -c, --create   Create the folder/bucket when it is missing
        -i, --immutable <mode>
                       Require none, governance or compliance immutability (S3 only),
                       and hold the backups written by this run in that mode
        -r, --retention <days>
                       Require a retention period of at least <days> days, needs -i
      Commands and parameters:
        list [<path>]
        backup <path> <name>
        restore <path> <name>
        remove <path> <name>
        crypto <path> <name>
        immutable [<path>]
Where:
    bucket/dir - The name of the bucket/folder that contains the objects.
    path - The path within the folder to list objects.
    mode - none, governance or compliance. The bucket has to apply at least
           the requested mode to new backups, otherwise btest stops here.
    days - the number of days new backups have to be retained for.
)" << std::endl;

    return 1;
}

int main(int argc, char* argv[])
{
    // Owns the AWS SDK lifecycle for the whole process, as required before any S3
    // backup manager is created. Being declared first, it is destroyed last and so
    // outlives the manager below.
    Abr::AwsSdkGuard awsSdkGuard;

    bool bFile = false;
    bool bCreateFolder = false;
    Abr::ImmutabilityMode immutability = Abr::ImmutabilityMode::NONE;
    int immutabilityDays = 0;

    while (argc > 1 && argv[1][0] == '-') {
        std::string arg = argv[1];
        if (arg == "-f" || arg == "--file") {
            bFile = true;
        } else if (arg == "-c" || arg == "--create") {
            bCreateFolder = true;
        } else if (arg == "-i" || arg == "--immutable") {
            if (argc < 3 || !ParseImmutabilityMode(argv[2], immutability)) {
                std::cerr << "Expected none, governance or compliance after " << arg << std::endl;
                return 1;
            }
            argc--;
            argv++;
        } else if (arg == "-r" || arg == "--retention") {
            if (argc < 3 || !ParseRetention(argv[2], immutabilityDays)) {
                std::cerr << "Expected a retention period in days after " << arg << std::endl;
                return 1;
            }
            argc--;
            argv++;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
        argc--;
        argv++;
    }

    // A retention period on its own is not a request the manager looks at: the
    // check starts from the mode, so asking for one without the other would
    // silently do nothing.
    if (immutabilityDays > 0 && immutability == Abr::ImmutabilityMode::NONE) {
        std::cerr << "--retention has no effect without --immutable" << std::endl;
        return 1;
    }

    if (argc < 3) {
        return PrintUsage();
    }

    std::shared_ptr<Abr::IBackupManager> backupManager;

    // The S3 manager reads its parameters back as S3BackupManagerParams, so the
    // mode decides which of the two structures has to be filled in.
    if (bFile) {
        // A local folder has no Object Lock to ask about, so a request for one
        // is a mistake worth pointing out rather than an option to drop.
        if (immutability != Abr::ImmutabilityMode::NONE) {
            std::cerr << "--immutable applies to S3 buckets only" << std::endl;
            return 1;
        }

        Abr::BackupManagerParams params;
        params.m_size = sizeof(params);
        params.m_createFolderIfMissing = bCreateFolder;
        params.m_folder = argv[1];

        backupManager = Abr::createBackupManager("file", params);
    } else {
        Abr::S3BackupManagerParams params;
        params.m_size = sizeof(params);
        params.m_createFolderIfMissing = bCreateFolder;
        params.m_folder = argv[1];
        params.m_pathStyle = true; // Use path-style addressing for S3 buckets

        // Checked while the manager initializes: it probes the bucket and gives
        // up when what is asked for here is more than the bucket applies.
        params.m_immutabilityCurrent = immutability;
        params.m_immutabilityDays = immutabilityDays;

        if (std::getenv("AWS_PROFILE")) {
            params.m_profile = std::getenv("AWS_PROFILE");
        } else {
            std::cerr << "AWS_PROFILE is not set!" << std::endl;
            return 1;
        }

        backupManager = Abr::createBackupManager("s3", params);
    }

    if (backupManager == nullptr) {
        std::cerr << "Failed to initialize backup manager" << std::endl;
        return 1;
    }

    std::string cmd = argv[2];

    if (cmd == "list") {
        return ListBackups(backupManager, argc > 3 ? argv[3] : "");
    }
    if (cmd == "backup") {
        if (argc < 5) {
            return PrintUsage();
        }
        return TestBackup(backupManager, argv[3], argv[4], immutability, immutabilityDays);
    }
    if (cmd == "restore") {
        if (argc < 5) {
            return PrintUsage();
        }
        return TestRestore(backupManager, argv[3], argv[4]);
    }
    if (cmd == "remove") {
        if (argc < 5) {
            return PrintUsage();
        }
        return TestRemove(backupManager, argv[3], argv[4]);
    }
    if (cmd == "crypto") {
        if (argc < 5) {
            return PrintUsage();
        }
        return TestEncryption(backupManager, argv[3], argv[4]);
    }
    if (cmd == "immutable") {
        return TestImmutability(backupManager, argc > 3 ? argv[3] : "");
    }

    return PrintUsage();
}

