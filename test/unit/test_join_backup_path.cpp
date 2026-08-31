// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for join_backup_path()
//
// Every file name in a repository is built by this function, so a change to it
// silently moves where backups are written and looked for.
//

#include <gtest/gtest.h>

#include "backup_core.h"

TEST(JoinBackupPath, JoinsPathAndName)
{
    EXPECT_EQ(join_backup_path("vm-01", "daily"), "vm-01/daily");
}

TEST(JoinBackupPath, EmptyPathYieldsBareName)
{
    // A backup at the root of the repository must not get a leading separator.
    EXPECT_EQ(join_backup_path("", "daily"), "daily");
}

TEST(JoinBackupPath, EmptyNameYieldsBarePath)
{
    EXPECT_EQ(join_backup_path("vm-01", ""), "vm-01");
}

TEST(JoinBackupPath, BothEmptyYieldsEmpty)
{
    EXPECT_EQ(join_backup_path("", ""), "");
}

TEST(JoinBackupPath, KeepsNestedPathsIntact)
{
    EXPECT_EQ(join_backup_path("tenant/vm-01", "daily"), "tenant/vm-01/daily");
}

TEST(JoinBackupPath, DoesNotNormaliseSeparators)
{
    // The function joins, it does not clean up: a caller supplying a trailing
    // separator gets a doubled one, which is worth knowing when reading a path
    // out of a repository listing.
    EXPECT_EQ(join_backup_path("vm-01/", "daily"), "vm-01//daily");
}
