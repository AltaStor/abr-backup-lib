// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for the immutability rules
//
// These are the two decisions behind the Object Lock check S3BackupManager runs
// while it initialises: what immutability a bucket really applies to new
// backups, and whether that covers what the caller asked for. Probing a live
// bucket belongs to the btest driven suite; the rules are settled here, where
// every configuration a bucket can be in is cheap to state.
//

#include <gtest/gtest.h>

#include <string>

#include "s3_backup.h"

namespace
{
    // The rules under test are static members of the S3 manager; the alias keeps
    // the calls below on one screen.
    using Manager = Abr::S3BackupManager;

    Abr::ObjectLockSettings lockedWith(Abr::ImmutabilityMode mode, int days)
    {
        Abr::ObjectLockSettings settings;

        settings.m_enabled = true;
        settings.m_defaultMode = mode;
        settings.m_days = days;

        return settings;
    }

    Abr::BackupFolderInfo resolved(const Abr::ObjectLockSettings &settings)
    {
        Abr::BackupFolderInfo effective;

        Manager::resolveImmutability(settings, effective);

        return effective;
    }

    Abr::ImmutabilityRequest requestOf(Abr::ImmutabilityMode mode, int days)
    {
        Abr::ImmutabilityRequest request;

        request.m_mode = mode;
        request.m_days = days;

        return request;
    }

    // ---- what the repository applies -------------------------------------

    TEST(ResolveImmutability, ReportsNothingWhenObjectLockIsOff)
    {
        // A bucket created without Object Lock cannot be given it afterwards, so
        // nothing about it is a matter of the default retention rule.
        Abr::ObjectLockSettings settings = lockedWith(Abr::ImmutabilityMode::COMPLIANCE, 30);
        settings.m_enabled = false;

        const auto effective = resolved(settings);

        EXPECT_EQ(effective.m_immutabilitySupported, Abr::ImmutabilityMode::NONE);
        EXPECT_EQ(effective.m_immutabilityCurrent, Abr::ImmutabilityMode::NONE);
        EXPECT_EQ(effective.m_immutabilityDays, 0);
    }

    TEST(ResolveImmutability, ReportsComplianceAsSupportedOnceObjectLockIsOn)
    {
        // The switch is what decides the ceiling: with it on, an object may be
        // retained in either mode, and compliance is the stronger of the two.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 7));

        EXPECT_EQ(effective.m_immutabilitySupported, Abr::ImmutabilityMode::COMPLIANCE);
    }

    TEST(ResolveImmutability, TakesTheCurrentModeAndPeriodFromTheDefaultRule)
    {
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 30));

        EXPECT_EQ(effective.m_immutabilityCurrent, Abr::ImmutabilityMode::GOVERNANCE);
        EXPECT_EQ(effective.m_immutabilityDays, 30);
    }

    TEST(ResolveImmutability, ReportsNoCurrentImmutabilityWithoutADefaultRule)
    {
        // Object Lock on, no rule to go with it: backups are written without
        // retention headers of their own, so they stay deletable.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::NONE, 0));

        EXPECT_EQ(effective.m_immutabilitySupported, Abr::ImmutabilityMode::COMPLIANCE);
        EXPECT_EQ(effective.m_immutabilityCurrent, Abr::ImmutabilityMode::NONE);
        EXPECT_EQ(effective.m_immutabilityDays, 0);
    }

    TEST(ResolveImmutability, DropsAModeThatComesWithNoPeriod)
    {
        // Half a rule retains nothing, and reporting the mode on its own would
        // claim an immutability no backup actually gets.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::COMPLIANCE, 0));

        EXPECT_EQ(effective.m_immutabilityCurrent, Abr::ImmutabilityMode::NONE);
        EXPECT_EQ(effective.m_immutabilityDays, 0);
    }

    TEST(ResolveImmutability, DropsAPeriodThatComesWithNoMode)
    {
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::NONE, 30));

        EXPECT_EQ(effective.m_immutabilityCurrent, Abr::ImmutabilityMode::NONE);
        EXPECT_EQ(effective.m_immutabilityDays, 0);
    }

    // ---- whether it covers the request -----------------------------------

    TEST(HonoursImmutabilityRequest, AcceptsAnythingWhenNothingWasRequested)
    {
        Abr::ObjectLockSettings settings;
        std::string reason;

        EXPECT_TRUE(Manager::honoursImmutabilityRequest(resolved(settings),
                                                        requestOf(Abr::ImmutabilityMode::NONE, 0), reason));
        EXPECT_TRUE(reason.empty());
    }

    TEST(HonoursImmutabilityRequest, RefusesARepositoryWithoutObjectLock)
    {
        Abr::ObjectLockSettings settings;
        std::string reason;

        EXPECT_FALSE(Manager::honoursImmutabilityRequest(resolved(settings),
                                                         requestOf(Abr::ImmutabilityMode::GOVERNANCE, 30), reason));
        EXPECT_EQ(reason, "does not support governance immutability");
    }

    TEST(HonoursImmutabilityRequest, RefusesObjectLockWithoutADefaultRule)
    {
        // The bucket could hold immutable backups, but as it stands it would not
        // apply anything to the ones written here.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::NONE, 0));
        std::string reason;

        EXPECT_FALSE(Manager::honoursImmutabilityRequest(effective,
                                                         requestOf(Abr::ImmutabilityMode::GOVERNANCE, 30), reason));
        EXPECT_EQ(reason, "applies no immutability to new backups, while governance was requested");
    }

    TEST(HonoursImmutabilityRequest, RefusesAWeakerModeThanRequested)
    {
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 30));
        std::string reason;

        EXPECT_FALSE(Manager::honoursImmutabilityRequest(effective,
                                                         requestOf(Abr::ImmutabilityMode::COMPLIANCE, 30), reason));
        EXPECT_EQ(reason, "applies governance immutability to new backups, while compliance was requested");
    }

    TEST(HonoursImmutabilityRequest, AcceptsAStrongerModeThanRequested)
    {
        // Compliance holds a backup at least as firmly as governance does, so a
        // caller asking for the weaker one has nothing to complain about.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::COMPLIANCE, 30));
        std::string reason;

        EXPECT_TRUE(Manager::honoursImmutabilityRequest(effective,
                                                        requestOf(Abr::ImmutabilityMode::GOVERNANCE, 30), reason));
    }

    TEST(HonoursImmutabilityRequest, AcceptsTheExactModeAndPeriodRequested)
    {
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::COMPLIANCE, 30));
        std::string reason;

        EXPECT_TRUE(Manager::honoursImmutabilityRequest(effective,
                                                        requestOf(Abr::ImmutabilityMode::COMPLIANCE, 30), reason));
        EXPECT_TRUE(reason.empty());
    }

    TEST(HonoursImmutabilityRequest, RefusesAShorterPeriodThanRequested)
    {
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 7));
        std::string reason;

        EXPECT_FALSE(Manager::honoursImmutabilityRequest(effective,
                                                         requestOf(Abr::ImmutabilityMode::GOVERNANCE, 30), reason));
        EXPECT_EQ(reason, "retains new backups for 7 day(s), less than the requested 30 day(s)");
    }

    TEST(HonoursImmutabilityRequest, AcceptsALongerPeriodThanRequested)
    {
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 365));
        std::string reason;

        EXPECT_TRUE(Manager::honoursImmutabilityRequest(effective,
                                                        requestOf(Abr::ImmutabilityMode::GOVERNANCE, 30), reason));
        EXPECT_TRUE(reason.empty());
    }

    TEST(HonoursImmutabilityRequest, AcceptsAModeRequestedWithoutAPeriod)
    {
        // Asking for compliance and no period in particular still requires a
        // rule to be in place, since a mode alone is never in effect.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::COMPLIANCE, 30));
        std::string reason;

        EXPECT_TRUE(Manager::honoursImmutabilityRequest(effective,
                                                        requestOf(Abr::ImmutabilityMode::COMPLIANCE, 0), reason));

        EXPECT_FALSE(Manager::honoursImmutabilityRequest(resolved(lockedWith(Abr::ImmutabilityMode::NONE, 0)),
                                                         requestOf(Abr::ImmutabilityMode::COMPLIANCE, 0), reason));
    }

    // ---- what one backup is written with ----------------------------------

    Abr::BackupInfo backupAsking(Abr::ImmutabilityMode mode, int days)
    {
        Abr::BackupInfo info = {};

        info.m_immutabilityMode = mode;
        info.m_immutabilityDays = days;

        return info;
    }

    TEST(ResolveBackupRetention, WritesNoRetentionWhenNoneWasAskedFor)
    {
        // Without a retention of its own the backup is left to the bucket's
        // default rule, whatever that rule happens to be.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::COMPLIANCE, 30));
        Abr::ObjectLockRetention retention;
        std::string reason;

        EXPECT_TRUE(Manager::resolveBackupRetention(effective, backupAsking(Abr::ImmutabilityMode::NONE, 0),
                                                    retention, reason));
        EXPECT_EQ(retention.m_mode, Abr::ImmutabilityMode::NONE);
        EXPECT_TRUE(reason.empty());
    }

    TEST(ResolveBackupRetention, RefusesAModeTheBucketCannotHold)
    {
        // Object Lock off: no object of the bucket can be retained in any mode.
        const auto effective = resolved(Abr::ObjectLockSettings());
        Abr::ObjectLockRetention retention;
        std::string reason;

        EXPECT_FALSE(Manager::resolveBackupRetention(effective,
                                                     backupAsking(Abr::ImmutabilityMode::GOVERNANCE, 30),
                                                     retention, reason));
        EXPECT_EQ(reason, "does not support governance immutability");
        EXPECT_EQ(retention.m_mode, Abr::ImmutabilityMode::NONE);
    }

    TEST(ResolveBackupRetention, RefusesAModeWithoutAPeriod)
    {
        // The bucket can hold it, but the backup never said for how long, and a
        // mode on its own retains nothing.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 30));
        Abr::ObjectLockRetention retention;
        std::string reason;

        EXPECT_FALSE(Manager::resolveBackupRetention(effective,
                                                     backupAsking(Abr::ImmutabilityMode::COMPLIANCE, 0),
                                                     retention, reason));
        EXPECT_EQ(reason, "cannot hold a backup in compliance mode for 0 day(s)");
    }

    TEST(ResolveBackupRetention, HoldsTheBackupForTheRequestedPeriod)
    {
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 7));
        Abr::ObjectLockRetention retention;
        std::string reason;

        EXPECT_TRUE(Manager::resolveBackupRetention(effective,
                                                    backupAsking(Abr::ImmutabilityMode::GOVERNANCE, 30),
                                                    retention, reason));
        EXPECT_EQ(retention.m_mode, Abr::ImmutabilityMode::GOVERNANCE);
        EXPECT_TRUE(reason.empty());

        // A retention put on an object is the backup's own, so it may outlast
        // the seven days the bucket's default rule would have given it. The
        // date is only checked to the day, since it is taken from the clock.
        const int64_t millis = retention.m_retainUntil.Millis() - Aws::Utils::DateTime::Now().Millis();

        EXPECT_GT(millis, static_cast<int64_t>(29) * 24 * 60 * 60 * 1000);
        EXPECT_LE(millis, static_cast<int64_t>(30) * 24 * 60 * 60 * 1000);
    }

    TEST(ResolveBackupRetention, AsksForComplianceOnAGovernanceBucket)
    {
        // Object Lock is a bucket wide switch: once it is on, an object may be
        // written in either mode, whichever one the default rule names.
        const auto effective = resolved(lockedWith(Abr::ImmutabilityMode::GOVERNANCE, 30));
        Abr::ObjectLockRetention retention;
        std::string reason;

        EXPECT_TRUE(Manager::resolveBackupRetention(effective,
                                                    backupAsking(Abr::ImmutabilityMode::COMPLIANCE, 30),
                                                    retention, reason));
        EXPECT_EQ(retention.m_mode, Abr::ImmutabilityMode::COMPLIANCE);
    }

    // ---- how the settings are spelled out --------------------------------

    TEST(ImmutabilityNames, SpellsOutTheModes)
    {
        EXPECT_STREQ(Abr::immutabilityModeName(Abr::ImmutabilityMode::NONE), "none");
        EXPECT_STREQ(Abr::immutabilityModeName(Abr::ImmutabilityMode::GOVERNANCE), "governance");
        EXPECT_STREQ(Abr::immutabilityModeName(Abr::ImmutabilityMode::COMPLIANCE), "compliance");
    }
}
