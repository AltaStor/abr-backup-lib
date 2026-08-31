// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for Abr::createBackupManager()
//
// The factory is the only entry point a caller of the library uses, so it has to
// refuse clearly rather than hand back a manager that is not usable. The "s3"
// mode is not covered here: it needs the AWS SDK to be initialised and a bucket
// to talk to, which belongs to the btest driven suite in scripts/tests.sh.
//

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "abr_backup.h"

#include "test_support.h"

namespace
{
    Abr::BackupManagerParams paramsFor(const std::string &folder, bool createIfMissing = false)
    {
        Abr::BackupManagerParams params;

        params.m_size = sizeof(params);
        params.m_createFolderIfMissing = createIfMissing;
        params.m_folder = folder;

        return params;
    }

    TEST(BackupManagerFactory, CreatesAFileManagerForAnExistingFolder)
    {
        ut::TempDir tempDir;

        auto manager = Abr::createBackupManager("file", paramsFor(tempDir.str()));

        ASSERT_NE(manager, nullptr);
        EXPECT_EQ(manager->getFolder(), tempDir.str());
    }

    TEST(BackupManagerFactory, CreatesTheFolderWhenTheParametersSaySo)
    {
        ut::TempDir tempDir;
        const auto folder = (tempDir / "made-by-the-factory").string();

        auto manager = Abr::createBackupManager("file", paramsFor(folder, true));

        ASSERT_NE(manager, nullptr);
        EXPECT_TRUE(std::filesystem::is_directory(folder));
    }

    TEST(BackupManagerFactory, ReturnsNothingForAnUnknownMode)
    {
        ut::TempDir tempDir;
        ut::CerrCapture cerr;

        EXPECT_EQ(Abr::createBackupManager("ftp", paramsFor(tempDir.str())), nullptr);
        EXPECT_TRUE(cerr.contains("Invalid backup mode")) << cerr.text();
    }

    TEST(BackupManagerFactory, ReturnsNothingForAnEmptyMode)
    {
        ut::TempDir tempDir;
        ut::CerrCapture cerr;

        EXPECT_EQ(Abr::createBackupManager("", paramsFor(tempDir.str())), nullptr);
    }

    TEST(BackupManagerFactory, ReturnsNothingWhenTheManagerCannotBeInitialized)
    {
        ut::TempDir tempDir;
        ut::CerrCapture cerr;

        // A manager that failed to initialize must not be handed out half set up.
        EXPECT_EQ(Abr::createBackupManager("file", paramsFor((tempDir / "missing").string())),
                  nullptr);
        EXPECT_TRUE(cerr.contains("Failed to initialize backup manager")) << cerr.text();
    }
}
