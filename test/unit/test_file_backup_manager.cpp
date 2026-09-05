// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for Abr::FileBackupManager
//
// Unlike the core tests these run against a real directory, because what they
// check is exactly what a filesystem provides: which paths are accepted as a
// repository, what permissions the created folders and files end up with, and
// what a listing and a delete see. Every test gets its own temporary directory,
// which is removed again when it ends.
//

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "abr_backup_format.h"
#include "file_backup.h"

#include "test_support.h"

namespace
{
    namespace fs = std::filesystem;

    const std::string kPath = "vm-01";
    const std::string kName = "daily";

    Abr::BackupInfo defaultInfo()
    {
        Abr::BackupInfo info = {};

        info.sectors = 0x10000;
        info.sectorSize = 512;
        info.phySectorSize = 512;

        return info;
    }

    class FileBackupManagerTest : public testing::Test
    {
    protected:
        // A manager on the temporary directory, ready to use.
        std::shared_ptr<Abr::FileBackupManager> openManager(bool createIfMissing = false,
                                                            const std::string &folder = "")
        {
            Abr::BackupManagerParams params;
            params.m_size = sizeof(params);
            params.m_createFolderIfMissing = createIfMissing;
            params.m_folder = folder.empty() ? tempDir.str() : folder;

            auto manager = std::make_shared<Abr::FileBackupManager>();
            if (!manager->initialize(params)) {
                return nullptr;
            }

            return manager;
        }

        // Writes and commits a backup holding one range of `original`.
        void writeBackup(const std::shared_ptr<Abr::FileBackupManager> &manager,
                         const std::vector<char> &original, const std::string &path = kPath,
                         const std::string &name = kName)
        {
            std::shared_ptr<Abr::IBackup> backup;
            ASSERT_TRUE(manager->createBackup(path, name, defaultInfo(), backup));

            auto bytes = original;
            ASSERT_TRUE(backup->saveDataRange(bytes.data(), static_cast<uint32_t>(bytes.size()), 0));
            ASSERT_TRUE(backup->finalizeBackup());
        }

        ut::TempDir tempDir;
    };

    //
    // Setting up a repository
    //

    TEST_F(FileBackupManagerTest, InitializeRejectsAnEmptyFolderName)
    {
        Abr::BackupManagerParams params;
        params.m_size = sizeof(params);
        params.m_createFolderIfMissing = true;
        params.m_folder = "";

        // Even with permission to create the folder there is nothing to create.
        ut::CerrCapture cerr;
        Abr::FileBackupManager manager;
        EXPECT_FALSE(manager.initialize(params));
        EXPECT_TRUE(cerr.contains("Folder name is empty")) << cerr.text();
    }

    TEST_F(FileBackupManagerTest, InitializeRejectsAMissingFolder)
    {
        const std::string missing = (tempDir / "not-there").string();

        ut::CerrCapture cerr;
        EXPECT_EQ(openManager(false, missing), nullptr);
        EXPECT_TRUE(cerr.contains("Directory does not exist")) << cerr.text();
        EXPECT_FALSE(fs::exists(missing)) << "a rejected folder must not be created anyway";
    }

    TEST_F(FileBackupManagerTest, InitializeCreatesTheFolderWhenAsked)
    {
        const fs::path created = tempDir / "new-repo";

        ASSERT_NE(openManager(true, created.string()), nullptr);

        ASSERT_TRUE(fs::is_directory(created));

        // A repository holds backup data, so it must not be readable by others.
        EXPECT_EQ(ut::modeOf(created), 0750u);
    }

    TEST_F(FileBackupManagerTest, InitializeCreatesEveryMissingParentProtected)
    {
        const fs::path nested = tempDir / "tenant" / "site" / "repo";

        ASSERT_NE(openManager(true, nested.string()), nullptr);

        // create_directories() would apply the umask and leave the parents world
        // readable, so each level is created and tightened in turn.
        EXPECT_EQ(ut::modeOf(tempDir / "tenant"), 0750u);
        EXPECT_EQ(ut::modeOf(tempDir.path() / "tenant" / "site"), 0750u);
        EXPECT_EQ(ut::modeOf(nested), 0750u);
    }

    TEST_F(FileBackupManagerTest, InitializeRejectsAPathThatIsNotADirectory)
    {
        const fs::path file = tempDir / "a-file";
        ASSERT_TRUE(ut::writeWholeFile(file, { 'x' }));

        ut::CerrCapture cerr;
        EXPECT_EQ(openManager(false, file.string()), nullptr);
        EXPECT_TRUE(cerr.contains("is not a directory")) << cerr.text();
    }

    TEST_F(FileBackupManagerTest, InitializeRejectsAFolderThatIsTooPermissive)
    {
        std::error_code ec;
        fs::permissions(tempDir.path(), fs::perms::all, fs::perm_options::replace, ec);
        ASSERT_FALSE(ec);

        // Without permission to fix the folder up, a world readable repository has
        // to be refused rather than used.
        ut::CerrCapture cerr;
        EXPECT_EQ(openManager(false), nullptr);
        EXPECT_TRUE(cerr.contains("expected 750 or stricter")) << cerr.text();
    }

    TEST_F(FileBackupManagerTest, InitializeTightensAFolderItWasAskedToSetUp)
    {
        std::error_code ec;
        fs::permissions(tempDir.path(), fs::perms::all, fs::perm_options::replace, ec);
        ASSERT_FALSE(ec);

        ut::CerrCapture cerr;
        ASSERT_NE(openManager(true), nullptr);

        EXPECT_EQ(ut::modeOf(tempDir.path()), 0750u);
        EXPECT_TRUE(cerr.contains("Changed permissions")) << cerr.text();
    }

    TEST_F(FileBackupManagerTest, InitializeKeepsAStricterFolderAsItIs)
    {
        std::error_code ec;
        fs::permissions(tempDir.path(), fs::perms::owner_all, fs::perm_options::replace, ec);
        ASSERT_FALSE(ec);

        // Only the excess is removed; a repository that is already more private
        // than required stays that way.
        ASSERT_NE(openManager(true), nullptr);
        EXPECT_EQ(ut::modeOf(tempDir.path()), 0700u);
    }

    TEST_F(FileBackupManagerTest, TheManagerReportsItsFolder)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        EXPECT_EQ(manager->getFolder(), tempDir.str());
    }

    //
    // Listing
    //

    TEST_F(FileBackupManagerTest, ListReturnsTheFinalizedBackups)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, ut::patternBuffer(0x1000, 0x1000), kPath, "daily"));
        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, ut::patternBuffer(0x1000, 0x1000), kPath, "weekly"));

        std::vector<std::string> backups;
        ASSERT_TRUE(manager->listBackups(kPath, backups));

        std::sort(backups.begin(), backups.end());
        EXPECT_EQ(backups, (std::vector<std::string>{ "daily", "weekly" }));
    }

    TEST_F(FileBackupManagerTest, ListIgnoresEverythingButTheIndexFiles)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, ut::patternBuffer(0x1000, 0x1000)));

        std::istringstream metaData("metadata");
        ASSERT_TRUE(manager->saveMetaData(kPath, kName, metaData));

        // The data and metadata files belong to the same backup, so the listing
        // has to name it once, by its index.
        std::vector<std::string> backups;
        ASSERT_TRUE(manager->listBackups(kPath, backups));
        EXPECT_EQ(backups, (std::vector<std::string>{ kName }));
    }

    TEST_F(FileBackupManagerTest, ListOfAMissingPathIsEmptyRatherThanAnError)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        std::vector<std::string> backups;
        EXPECT_TRUE(manager->listBackups("no-such-path", backups));
        EXPECT_TRUE(backups.empty());
    }

    //
    // Backing up and restoring
    //

    TEST_F(FileBackupManagerTest, ABackupRoundTripsThroughTheFilesystem)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        const auto original = ut::patternBuffer(0x2000, 0x2000);
        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, original));

        std::shared_ptr<Abr::IRestore> restore;
        ASSERT_TRUE(manager->openBackup(kPath, kName, restore));

        std::vector<Abr::Range> ranges;
        ASSERT_TRUE(restore->getDataRanges(ranges));
        ASSERT_EQ(ranges.size(), 1u);
        EXPECT_EQ(ranges[0].length, original.size());

        std::vector<char> loaded(ranges[0].length, 0);
        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_EQ(loaded, original);

        EXPECT_TRUE(restore->close());
    }

    //
    // A backup can end up with no range at all in two ways: everything it was
    // given was empty and the empty block elimination dropped it, or it was
    // never given anything, which is what a no-change incremental looks like.
    // Both are valid backups made of an index without records and an empty data
    // file, and both have to round trip like any other backup.
    //

    TEST_F(FileBackupManagerTest, ABackupOfAnAllZeroImageRoundTripsThroughTheFilesystem)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, std::vector<char>(0x2000, 0)));

        const fs::path indexPath = tempDir.path() / kPath / (kName + Abr::INDEX_FILE_EXT);
        const fs::path dataPath = tempDir.path() / kPath / (kName + Abr::DATA_FILE_EXT);

        // The index is a header without a single record, and the data file is
        // there and empty rather than missing.
        ASSERT_TRUE(fs::exists(indexPath));
        ASSERT_TRUE(fs::exists(dataPath));
        EXPECT_EQ(fs::file_size(indexPath), Abr::INDEX_FILE_HEADER_SIZE);
        EXPECT_EQ(fs::file_size(dataPath), 0u);

        std::vector<std::string> backups;
        ASSERT_TRUE(manager->listBackups(kPath, backups));
        EXPECT_EQ(backups, (std::vector<std::string>{ kName }));

        std::shared_ptr<Abr::IRestore> restore;
        ASSERT_TRUE(manager->openBackup(kPath, kName, restore));

        std::vector<Abr::Range> ranges;
        ASSERT_TRUE(restore->getDataRanges(ranges));
        EXPECT_TRUE(ranges.empty());

        const auto &info = restore->getInfo();
        EXPECT_EQ(info.sectors, defaultInfo().sectors);
        EXPECT_EQ(info.orig_data_size, 0u);
        EXPECT_EQ(info.stored_data_size, 0u);

        // There is no range to ask for, so the restore and the verify path both
        // have to turn a request down instead of reading the empty data file.
        std::vector<char> loaded(0x1000, 0);
        std::vector<char> tempBuf;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_FALSE(restore->checkDataRange(0, tempBuf));

        EXPECT_TRUE(restore->close());

        EXPECT_TRUE(manager->deleteBackup(kPath, kName));
        EXPECT_FALSE(fs::exists(indexPath));
        EXPECT_FALSE(fs::exists(dataPath));
    }

    TEST_F(FileBackupManagerTest, ABackupThatWasGivenNothingRoundTripsThroughTheFilesystem)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        // What a no-change incremental writes: created and committed without a
        // single saveDataRange() call.
        Abr::BackupInfo info = defaultInfo();
        info.incremental = true;

        std::shared_ptr<Abr::IBackup> backup;
        ASSERT_TRUE(manager->createBackup(kPath, kName, info, backup));
        ASSERT_TRUE(backup->finalizeBackup());
        backup.reset();

        const fs::path dataPath = tempDir.path() / kPath / (kName + Abr::DATA_FILE_EXT);
        ASSERT_TRUE(fs::exists(dataPath));
        EXPECT_EQ(fs::file_size(dataPath), 0u);

        std::shared_ptr<Abr::IRestore> restore;
        ASSERT_TRUE(manager->openBackup(kPath, kName, restore));

        std::vector<Abr::Range> ranges;
        ASSERT_TRUE(restore->getDataRanges(ranges));
        EXPECT_TRUE(ranges.empty());
        EXPECT_TRUE(restore->getInfo().incremental);

        // remove() takes the empty data file with it, the same way it does for a
        // backup that stored something.
        EXPECT_TRUE(restore->remove());
        EXPECT_FALSE(fs::exists(tempDir.path() / kPath / (kName + Abr::INDEX_FILE_EXT)));
        EXPECT_FALSE(fs::exists(dataPath));
    }

    TEST_F(FileBackupManagerTest, TheBackupFilesAreNotReadableByOthers)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, ut::patternBuffer(0x1000, 0x1000)));

        // ofstream would create these as 0666 & ~umask; the backend tightens them.
        EXPECT_EQ(ut::modeOf(tempDir.path() / kPath / (kName + Abr::INDEX_FILE_EXT)), 0640u);
        EXPECT_EQ(ut::modeOf(tempDir.path() / kPath / (kName + Abr::DATA_FILE_EXT)), 0640u);
    }

    TEST_F(FileBackupManagerTest, ANestedPathIsCreatedProtected)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ASSERT_NO_FATAL_FAILURE(
            writeBackup(manager, ut::patternBuffer(0x1000, 0x1000), "tenant/vm-07"));

        EXPECT_EQ(ut::modeOf(tempDir.path() / "tenant"), 0750u);
        EXPECT_EQ(ut::modeOf(tempDir.path() / "tenant" / "vm-07"), 0750u);

        std::shared_ptr<Abr::IRestore> restore;
        ASSERT_TRUE(manager->openBackup("tenant/vm-07", kName, restore));
        EXPECT_TRUE(restore->close());
    }

    TEST_F(FileBackupManagerTest, OpeningAMissingBackupFails)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ut::CerrCapture cerr;
        std::shared_ptr<Abr::IRestore> restore;
        EXPECT_FALSE(manager->openBackup(kPath, "no-such-backup", restore));
        EXPECT_EQ(restore, nullptr);
    }

    TEST_F(FileBackupManagerTest, CreatingABackupThatAlreadyExistsFails)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, ut::patternBuffer(0x1000, 0x1000)));

        ut::CerrCapture cerr;
        std::shared_ptr<Abr::IBackup> backup;
        EXPECT_FALSE(manager->createBackup(kPath, kName, defaultInfo(), backup));

        // The backup that was already there has to survive the refused attempt.
        std::shared_ptr<Abr::IRestore> restore;
        ASSERT_TRUE(manager->openBackup(kPath, kName, restore));
        EXPECT_TRUE(restore->close());
    }

    TEST_F(FileBackupManagerTest, AnAbandonedBackupLeavesNoFilesBehind)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        {
            std::shared_ptr<Abr::IBackup> backup;
            ASSERT_TRUE(manager->createBackup(kPath, kName, defaultInfo(), backup));

            auto bytes = ut::patternBuffer(0x1000, 0x1000);
            ASSERT_TRUE(backup->saveDataRange(bytes.data(), bytes.size(), 0));

            EXPECT_TRUE(fs::exists(tempDir.path() / kPath / (kName + Abr::INDEX_FILE_EXT)));
        }

        // Dropping the backup without finalizing it is a caller giving up, and
        // what it wrote must not be left in the repository.
        EXPECT_FALSE(fs::exists(tempDir.path() / kPath / (kName + Abr::INDEX_FILE_EXT)));
        EXPECT_FALSE(fs::exists(tempDir.path() / kPath / (kName + Abr::DATA_FILE_EXT)));

        std::vector<std::string> backups;
        ASSERT_TRUE(manager->listBackups(kPath, backups));
        EXPECT_TRUE(backups.empty());
    }

    //
    // Metadata
    //

    TEST_F(FileBackupManagerTest, MetadataRoundTrips)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        std::istringstream in("some metadata for the backup");
        ASSERT_TRUE(manager->saveMetaData(kPath, kName, in));

        std::ostringstream out;
        ASSERT_TRUE(manager->loadMetaData(kPath, kName, out));
        EXPECT_EQ(out.str(), "some metadata for the backup");
    }

    TEST_F(FileBackupManagerTest, EmptyMetadataRoundTrips)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        // Inserting a streambuf that transfers nothing raises failbit, which must
        // not be mistaken for a write error: the save has already truncated any
        // metadata that was stored before.
        std::istringstream in("");
        ASSERT_TRUE(manager->saveMetaData(kPath, kName, in));

        std::ostringstream out;
        EXPECT_TRUE(manager->loadMetaData(kPath, kName, out));
        EXPECT_EQ(out.str(), "");
    }

    TEST_F(FileBackupManagerTest, SavingMetadataReplacesWhatWasStored)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        std::istringstream first("a much longer first version of the metadata");
        ASSERT_TRUE(manager->saveMetaData(kPath, kName, first));

        std::istringstream second("short");
        ASSERT_TRUE(manager->saveMetaData(kPath, kName, second));

        std::ostringstream out;
        ASSERT_TRUE(manager->loadMetaData(kPath, kName, out));
        EXPECT_EQ(out.str(), "short") << "the previous metadata was not truncated";
    }

    TEST_F(FileBackupManagerTest, SavingMetadataCreatesTheMissingPath)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        std::istringstream in("metadata");
        ASSERT_TRUE(manager->saveMetaData("brand/new/path", kName, in));

        EXPECT_TRUE(fs::exists(tempDir.path() / "brand" / "new" / "path" / (kName + Abr::META_FILE_EXT)));
    }

    TEST_F(FileBackupManagerTest, LoadingMissingMetadataFails)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ut::CerrCapture cerr;
        std::ostringstream out;
        EXPECT_FALSE(manager->loadMetaData(kPath, "no-such-backup", out));
        EXPECT_TRUE(cerr.contains("Failed to open file")) << cerr.text();
    }

    //
    // Deleting
    //

    TEST_F(FileBackupManagerTest, DeleteRemovesIndexDataAndMetadata)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ASSERT_NO_FATAL_FAILURE(writeBackup(manager, ut::patternBuffer(0x1000, 0x1000)));

        std::istringstream in("metadata");
        ASSERT_TRUE(manager->saveMetaData(kPath, kName, in));

        EXPECT_TRUE(manager->deleteBackup(kPath, kName));

        for (const char *ext : { Abr::INDEX_FILE_EXT, Abr::DATA_FILE_EXT, Abr::META_FILE_EXT }) {
            EXPECT_FALSE(fs::exists(tempDir.path() / kPath / (kName + ext))) << "left behind: " << ext;
        }
    }

    TEST_F(FileBackupManagerTest, DeleteFailsWhenThereIsNothingToDelete)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        ut::CerrCapture cerr;
        EXPECT_FALSE(manager->deleteBackup(kPath, "no-such-backup"));
        EXPECT_TRUE(cerr.contains("not found")) << cerr.text();
    }

    TEST_F(FileBackupManagerTest, DeleteRemovesTheLeftoversOfAnInterruptedRun)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        // What a run that died before finalizing leaves behind has no readable
        // index, so deleting by name is the only way to get rid of it.
        const fs::path orphan = tempDir.path() / kPath / (kName + Abr::DATA_FILE_EXT);
        fs::create_directories(orphan.parent_path());
        ASSERT_TRUE(ut::writeWholeFile(orphan, std::vector<char>(64, 'x')));

        EXPECT_TRUE(manager->deleteBackup(kPath, kName));
        EXPECT_FALSE(fs::exists(orphan));
    }

    TEST_F(FileBackupManagerTest, ReportsNoImmutability)
    {
        auto manager = openManager();
        ASSERT_NE(manager, nullptr);

        // A local folder has no Object Lock to offer, and the manager inherits
        // the default answer. A caller reading this must not be left with the
        // impression that a backup put here cannot be deleted again.
        Abr::BackupFolderInfo info;

        manager->getFolderInfo(kPath, info);

        EXPECT_EQ(info.m_immutabilitySupported, Abr::ImmutabilityMode::NONE);
        EXPECT_EQ(info.m_immutabilityCurrent, Abr::ImmutabilityMode::NONE);
        EXPECT_EQ(info.m_immutabilityDays, 0);
    }
}
