// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for Abr::RestoreCore
//
// The restore side is what stands between a damaged repository and a silently
// wrong restore, so most of these tests damage a backup on purpose - a flipped
// byte in the header, in an index record or in the stored data - and assert
// that opening or loading it fails instead of returning something plausible.
//

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crc32c/crc32c.h"

#include "core_fixture.h"
#include "memory_backend.h"
#include "test_support.h"

namespace
{
    class RestoreCoreTest : public ut::CoreTest
    {
    protected:
        // The backup most of these tests restore from: two full blocks of
        // distinct data, so a mix-up between ranges shows up as a comparison
        // failure rather than as a coincidence.
        void buildDefaultBackup()
        {
            writeBackup(ut::defaultInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000, 'A') },
                                             { 0x8000, ut::patternBuffer(0x1000, 0x1000, 'N') } });
        }
    };

    //
    // Opening
    //

    TEST_F(RestoreCoreTest, OpenRestoresTheBackupInfo)
    {
        auto info = ut::defaultInfo();
        info.sectors = 0x40000;
        info.minDataBlock = 0x1000;
        info.maxDataBlock = 0x8000;

        writeBackup(info, { { 0, ut::patternBuffer(0x2000, 0x2000) } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        const auto &restored = restore->getInfo();
        EXPECT_EQ(restored.sectors, info.sectors);
        EXPECT_EQ(restored.sectorSize, info.sectorSize);
        EXPECT_EQ(restored.phySectorSize, info.phySectorSize);
        EXPECT_EQ(restored.minDataBlock, info.minDataBlock);
        EXPECT_EQ(restored.maxDataBlock, info.maxDataBlock);
        EXPECT_EQ(restored.compression, 0u);
        EXPECT_EQ(restored.encryption, 0u);
        EXPECT_EQ(restored.incremental, 0u);
        EXPECT_EQ(restored.orig_data_size, 0x2000u);
        EXPECT_EQ(restored.stored_data_size, 0x2000u);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, OpenReportsTheIncrementalFlag)
    {
        auto info = ut::defaultInfo();
        info.incremental = 1;

        writeBackup(info, { { 0, ut::patternBuffer(0x1000, 0x1000) } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        // An incremental is not restorable on its own, so a caller has to be able
        // to tell one apart from a full backup.
        EXPECT_EQ(restore->getInfo().incremental, 1u);
        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, OpenFailsWhenTheIndexIsMissing)
    {
        ut::CerrCapture cerr;

        EXPECT_FALSE(newRestore()->open(kPath, "no-such-backup"));
        EXPECT_TRUE(cerr.contains("Failed to open index file")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, OpenFailsOnAForeignSignature)
    {
        buildDefaultBackup();
        patchHeader<char>(0, 'X');

        ut::CerrCapture cerr;
        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_TRUE(cerr.contains("Invalid index file signature")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, OpenFailsOnAnUnknownFormatVersion)
    {
        buildDefaultBackup();
        patchHeader<uint16_t>(offsetof(Abr::IndexHeader, version), Abr::INDEX_FILE_VERSION + 1);

        ut::CerrCapture cerr;
        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_TRUE(cerr.contains("Invalid index file version")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, OpenFailsWhenTheHeaderChecksumDoesNotMatch)
    {
        buildDefaultBackup();

        // Changing the recorded image size without resealing is exactly the
        // damage the header checksum exists to catch.
        patchHeader<uint64_t>(offsetof(Abr::IndexHeader, image_size), 0x1234);

        ut::CerrCapture cerr;
        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_TRUE(cerr.contains("Header CRC32C mismatch")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, OpenFailsWhenAnIndexRecordIsCorrupted)
    {
        buildDefaultBackup();

        // The record area sits past the header, so the header checksum still
        // matches and only the index checksum can catch this.
        ASSERT_TRUE(repo.corrupt(indexFile(), Abr::INDEX_FILE_HEADER_SIZE + 4));

        ut::CerrCapture cerr;
        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_TRUE(cerr.contains("Index records CRC32C mismatch")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, OpenFailsOnAnInvalidSectorSize)
    {
        buildDefaultBackup();

        patchHeader<uint16_t>(offsetof(Abr::IndexHeader, sector_size), 1024);
        resealHeader();

        ut::CerrCapture cerr;
        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_TRUE(cerr.contains("Invalid sector size")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, OpenFailsWhenTheDataFileIsMissing)
    {
        buildDefaultBackup();
        repo.files.erase(dataFile());

        ut::CerrCapture cerr;
        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_TRUE(cerr.contains("Failed to open data file")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, OpenSucceedsForABackupWithoutAnyRecords)
    {
        // A backup whose data was entirely empty has no index record, but it is
        // a backup like any other: its data file is empty, not absent, and it is
        // opened the same way.
        writeBackup(ut::defaultInfo(), { { 0, std::vector<char>(0x2000, 0) } });
        ASSERT_TRUE(repo.has(dataFile()));
        ASSERT_EQ(repo.sizeOf(dataFile()), 0u);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<Abr::Range> ranges;
        EXPECT_TRUE(restore->getDataRanges(ranges));
        EXPECT_TRUE(ranges.empty());
        EXPECT_EQ(restore->indexRecordCount(), 0u);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, OpenFailsWhenTheDataFileOfAnEmptyBackupIsMissing)
    {
        // The empty data file is part of the backup, so a repository that lost it
        // is as broken as one that lost the data of a backup that stored something.
        writeBackup(ut::defaultInfo(), { { 0, std::vector<char>(0x2000, 0) } });
        repo.files.erase(dataFile());

        ut::CerrCapture cerr;
        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_TRUE(cerr.contains("Failed to open data file")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, LoadAndCheckAreRefusedForABackupWithoutAnyRecords)
    {
        // There is no range to ask for, so both the restore and the verify path
        // have to turn the request down instead of reading the empty data file.
        writeBackup(ut::defaultInfo(), { { 0, std::vector<char>(0x2000, 0) } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> loaded(0x1000, 0);
        std::vector<char> tempBuf;

        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_FALSE(restore->checkDataRange(0, tempBuf));

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, RemoveDeletesTheEmptyDataFileOfABackupWithoutAnyRecords)
    {
        writeBackup(ut::defaultInfo(), { { 0, std::vector<char>(0x2000, 0) } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        EXPECT_TRUE(restore->remove());
        EXPECT_FALSE(repo.has(indexFile()));
        EXPECT_FALSE(repo.has(dataFile()));
    }

    //
    // Enumerating ranges
    //

    TEST_F(RestoreCoreTest, DataRangesDescribeTheOriginalExtents)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<Abr::Range> ranges;
        ASSERT_TRUE(restore->getDataRanges(ranges));

        ASSERT_EQ(ranges.size(), 2u);
        EXPECT_EQ(ranges[0].offset, 0u);
        EXPECT_EQ(ranges[0].length, 0x1000u);
        EXPECT_EQ(ranges[1].offset, 0x8000u);
        EXPECT_EQ(ranges[1].length, 0x1000u);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, DataRangesRefuseToAppendToANonEmptyVector)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        // The range index a caller passes to loadDataRange() is a position in
        // this vector, so appending to a vector that already holds something
        // would silently shift every index.
        std::vector<Abr::Range> ranges{ Abr::Range(0, 0) };
        EXPECT_FALSE(restore->getDataRanges(ranges));

        EXPECT_TRUE(restore->close());
    }

    //
    // Loading data
    //

    TEST_F(RestoreCoreTest, LoadReturnsTheOriginalBytes)
    {
        const auto first = ut::patternBuffer(0x1000, 0x1000, 'A');
        const auto second = ut::patternBuffer(0x1000, 0x1000, 'N');

        writeBackup(ut::defaultInfo(), { { 0, first }, { 0x8000, second } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> loaded(0x1000, 0);

        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_EQ(loaded, first);

        ASSERT_TRUE(restore->loadDataRange(1, loaded.data(), loaded.size(), 0));
        EXPECT_EQ(loaded, second);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, LoadReturnsASliceOfAChunk)
    {
        const auto original = ut::patternBuffer(0x1000, 0x1000);
        writeBackup(ut::defaultInfo(), { { 0, original } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        // A partial read has to land on the right bytes of the chunk, not on its
        // beginning.
        std::vector<char> loaded(0x200, 0);
        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0x100));

        EXPECT_EQ(memcmp(loaded.data(), original.data() + 0x100, loaded.size()), 0);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, LoadRejectsARangeIndexBeyondTheIndex)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> loaded(0x1000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(2, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("Invalid range index")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, LoadRejectsAReadPastTheEndOfTheChunk)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> loaded(0x1000, 0);

        // The chunk is 0x1000 bytes, so 0x100 bytes from offset 0xf80 runs past
        // its end and must be refused rather than served from the next chunk.
        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), 0x100, 0xf80));
        EXPECT_TRUE(cerr.contains("Invalid data len")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, LoadRejectsALengthAndOffsetThatWrapAround)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> loaded(0x1000, 0);

        // length + offset wraps to zero in 32 bits. Compared as a sum that reads
        // as a request for nothing and reaches the copy with a length of nearly
        // 4GB; it has to be refused on its own terms instead.
        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), 0xffffffffu, 1));
        EXPECT_TRUE(cerr.contains("Invalid data len")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, LoadAcceptsAReadEndingExactlyAtTheChunkEnd)
    {
        const auto original = ut::patternBuffer(0x1000, 0x1000);
        writeBackup(ut::defaultInfo(), { { 0, original } });

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        // The boundary the tightened check must still let through.
        std::vector<char> loaded(0x100, 0);
        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0xf00));

        EXPECT_EQ(memcmp(loaded.data(), original.data() + 0xf00, loaded.size()), 0);
        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, LoadFailsWhenTheStoredChunkIsCorrupted)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        // A single flipped byte in the data file has to surface as a failure, not
        // as a restore that quietly returns wrong data.
        ASSERT_TRUE(repo.corrupt(dataFile(), 0x40));

        std::vector<char> loaded(0x1000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("CRC32C mismatch")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, LoadFailsWhenTheDataFileIsTruncated)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        repo.find(dataFile())->resize(0x100);

        std::vector<char> loaded(0x1000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("Failed to read from data file")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    //
    // Verifying without restoring
    //

    TEST_F(RestoreCoreTest, CheckAcceptsAnIntactChunk)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> tempBuf;
        EXPECT_TRUE(restore->checkDataRange(0, tempBuf));
        EXPECT_TRUE(restore->checkDataRange(1, tempBuf));

        // The buffer is grown as needed and reused across calls.
        EXPECT_GE(tempBuf.size(), 0x1000u);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, CheckRejectsACorruptedChunk)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        ASSERT_TRUE(repo.corrupt(dataFile(), 0x1080));

        std::vector<char> tempBuf;
        EXPECT_TRUE(restore->checkDataRange(0, tempBuf)) << "the first chunk is untouched";
        EXPECT_FALSE(restore->checkDataRange(1, tempBuf));

        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, CheckRejectsARangeIndexBeyondTheIndex)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> tempBuf;
        EXPECT_FALSE(restore->checkDataRange(99, tempBuf));

        EXPECT_TRUE(restore->close());
    }

    //
    // Closing and removing
    //

    TEST_F(RestoreCoreTest, CloseCanBeCalledTwice)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        EXPECT_TRUE(restore->close());
        EXPECT_TRUE(restore->close());
    }

    TEST_F(RestoreCoreTest, RemoveDeletesBothFiles)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        EXPECT_TRUE(restore->remove());
        EXPECT_FALSE(repo.has(indexFile()));
        EXPECT_FALSE(repo.has(dataFile()));
    }

    TEST_F(RestoreCoreTest, RemoveFailsWhenTheDataFileIsAlreadyGone)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        restore->close();
        repo.files.erase(dataFile());

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->remove());
        EXPECT_TRUE(cerr.contains("Failed to delete data file")) << cerr.text();
    }

    TEST_F(RestoreCoreTest, RemoveReportsAFailedDelete)
    {
        buildDefaultBackup();

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        repo.failDeleteOf = indexFile();

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->remove());
        EXPECT_TRUE(cerr.contains("Failed to delete index file")) << cerr.text();

        // The data file went first, so what is left is an index without data.
        EXPECT_FALSE(repo.has(dataFile()));
        repo.failDeleteOf.clear();
    }

    //
    // A rejected backup must not cost a file handle: open() validates the header
    // after opening the index file, so every one of those checks has to close it
    // on the way out.
    //

    TEST_F(RestoreCoreTest, OpenClosesTheIndexWhenTheCompressionIsUnsupported)
    {
        buildDefaultBackup();

        patchHeader<uint8_t>(offsetof(Abr::IndexHeader, compression), 23);
        resealHeader();

        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_EQ(repo.openHandles, 0) << "the index file handle was leaked";
    }

    TEST_F(RestoreCoreTest, OpenClosesTheIndexWhenTheEncryptionIsUnsupported)
    {
        buildDefaultBackup();

        patchHeader<uint8_t>(offsetof(Abr::IndexHeader, encryption), 2);
        resealHeader();

        EXPECT_FALSE(newRestore()->open(kPath, kName));
        EXPECT_EQ(repo.openHandles, 0) << "the index file handle was leaked";
    }
}
