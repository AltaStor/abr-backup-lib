// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for compressed backups
//
// Compression changes what reaches storage but must not change what comes back,
// so these tests assert the round trip first and the stored form second: the
// bytes on storage are not the plaintext, the checksums cover the compressed
// form, and damage to it is still caught.
//

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core_fixture.h"

namespace
{
    class CompressionTest : public ut::CoreTest
    {
    protected:
        static Abr::BackupInfo compressedInfo(uint8_t level)
        {
            auto info = ut::defaultInfo();
            info.compression = level;
            return info;
        }

        // Writes one range, reads it back through a restore and returns what came
        // out, so a round trip is a single comparison in the test body.
        std::vector<char> roundTrip(const Abr::BackupInfo &info, const std::vector<char> &original)
        {
            writeBackup(info, { { 0, original } });

            std::vector<char> loaded;

            auto restore = newRestore();
            EXPECT_TRUE(restore->open(kPath, kName));

            std::vector<Abr::Range> ranges;
            EXPECT_TRUE(restore->getDataRanges(ranges));

            if (ranges.size() == 1) {
                loaded.resize(ranges[0].length);
                EXPECT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
            } else {
                ADD_FAILURE() << "expected a single range, got " << ranges.size();
            }

            EXPECT_TRUE(restore->close());

            return loaded;
        }
    };

    TEST_F(CompressionTest, ACompressedBackupRoundTrips)
    {
        const auto original = ut::patternBuffer(0x2000, 0x2000);

        EXPECT_EQ(roundTrip(compressedInfo(3), original), original);
    }

    TEST_F(CompressionTest, EverySupportedLevelRoundTrips)
    {
        // The ends of the zstd range and a few points in between; the level only
        // has to survive the trip, not compress by any particular amount.
        for (uint8_t level : { 1, 3, 9, 19, 22 }) {
            ut::MemoryRepo levelRepo;
            std::swap(repo, levelRepo);

            const auto original = ut::patternBuffer(0x2000, 0x2000);
            EXPECT_EQ(roundTrip(compressedInfo(level), original), original) << "level " << int(level);

            std::swap(repo, levelRepo);
        }
    }

    TEST_F(CompressionTest, IncompressibleDataRoundTrips)
    {
        // Random bytes make zstd fall back to a stored block, which is the other
        // branch of the write path.
        const auto original = ut::randomBuffer(0x2000, 0x5eed);

        EXPECT_EQ(roundTrip(compressedInfo(9), original), original);
    }

    TEST_F(CompressionTest, TheHeaderRecordsTheCompressionLevel)
    {
        writeBackup(compressedInfo(7), { { 0, ut::patternBuffer(0x1000, 0x1000) } });

        // A restore has no other way of knowing that the chunks need decompressing.
        EXPECT_EQ(readHeader().compression, 7u);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));
        EXPECT_EQ(restore->getInfo().compression, 7u);
        EXPECT_TRUE(restore->close());
    }

    TEST_F(CompressionTest, CompressibleDataIsActuallyShrunk)
    {
        const auto original = ut::patternBuffer(0x4000, 0x4000);
        writeBackup(compressedInfo(9), { { 0, original } });

        const auto records = readRecords();
        ASSERT_EQ(records.size(), 1u);

        EXPECT_LT(records[0].chunk_length, records[0].orig_length);
        EXPECT_EQ(records[0].orig_length, 0x4000u);
        EXPECT_LT(repo.sizeOf(dataFile()), original.size());

        const auto header = readHeader();
        EXPECT_EQ(header.orig_data_size, 0x4000u);
        EXPECT_LT(header.stored_data_size, header.orig_data_size);
    }

    TEST_F(CompressionTest, TheStoredBytesAreNotThePlaintext)
    {
        const auto original = ut::patternBuffer(0x2000, 0x2000);
        writeBackup(compressedInfo(3), { { 0, original } });

        const auto *stored = repo.find(dataFile());
        ASSERT_NE(stored, nullptr);
        ASSERT_FALSE(stored->empty());

        EXPECT_NE(*stored, original);
    }

    TEST_F(CompressionTest, EmptyBlocksAreStillEliminated)
    {
        // Compression does not replace the empty block elimination: a half empty
        // range still produces one record covering only the half with data.
        writeBackup(compressedInfo(3), { { 0, ut::patternBuffer(0x2000, 0x1000) } });

        const auto records = readRecords();
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].orig_length, 0x1000u);
    }

    TEST_F(CompressionTest, APartialReadOfACompressedChunkReturnsTheRightSlice)
    {
        const auto original = ut::patternBuffer(0x2000, 0x2000);
        writeBackup(compressedInfo(3), { { 0, original } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        // The whole chunk has to be decompressed to serve a slice out of its
        // middle, so this exercises a path the full read does not.
        std::vector<char> loaded(0x400, 0);
        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0x800));

        EXPECT_EQ(memcmp(loaded.data(), original.data() + 0x800, loaded.size()), 0);
        EXPECT_TRUE(restore->close());
    }

    TEST_F(CompressionTest, TheChecksumCoversTheCompressedForm)
    {
        writeBackup(compressedInfo(3), { { 0, ut::patternBuffer(0x2000, 0x2000) } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        ASSERT_TRUE(repo.corrupt(dataFile(), 8));

        std::vector<char> loaded(0x2000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("CRC32C mismatch")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(CompressionTest, ADamagedFrameIsRejectedEvenWithAMatchingChecksum)
    {
        writeBackup(compressedInfo(3), { { 0, ut::patternBuffer(0x2000, 0x2000) } });

        // Destroy the zstd frame header and repair every checksum over it, so
        // nothing but the decompression itself can catch the damage.
        auto *stored = repo.find(dataFile());
        ASSERT_NE(stored, nullptr);
        ASSERT_GE(stored->size(), 4u);
        memset(stored->data(), 0x7f, 4);
        resealChunk(0);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> loaded(0x2000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("ZSTD decompression error")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(CompressionTest, CheckDataRangeValidatesTheCompressedChunk)
    {
        writeBackup(compressedInfo(3), { { 0, ut::patternBuffer(0x2000, 0x2000) } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> tempBuf;
        EXPECT_TRUE(restore->checkDataRange(0, tempBuf));

        ASSERT_TRUE(repo.corrupt(dataFile(), 12));
        EXPECT_FALSE(restore->checkDataRange(0, tempBuf));

        EXPECT_TRUE(restore->close());
    }
}
