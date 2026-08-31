// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for Abr::BackupCore
//
// The core holds everything that decides what a backup looks like on storage:
// the parameter validation, the empty block elimination, the chunk splitting,
// the checksums and the index header. It reaches storage only through four
// virtual file operations, so these tests run it against an in-memory
// repository and assert on the bytes it produced.
//

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "crc32c/crc32c.h"

#include "core_fixture.h"
#include "memory_backend.h"
#include "test_support.h"

namespace
{
    class BackupCoreTest : public ut::CoreTest
    {
    protected:
        std::unique_ptr<ut::MemoryBackup> newBackup()
        {
            return std::make_unique<ut::MemoryBackup>(repo, manager);
        }
    };

    //
    // Parameter validation
    //

    TEST_F(BackupCoreTest, CreateRejectsAnEmptyImage)
    {
        auto info = ut::defaultInfo();
        info.sectors = 0;

        EXPECT_FALSE(newBackup()->create(kPath, kName, info));
        EXPECT_TRUE(repo.files.empty()) << "a rejected backup must not touch the repository";
    }

    TEST_F(BackupCoreTest, CreateRejectsAZeroSectorSize)
    {
        auto info = ut::defaultInfo();
        info.sectorSize = 0;

        EXPECT_FALSE(newBackup()->create(kPath, kName, info));
        EXPECT_TRUE(repo.files.empty());
    }

    TEST_F(BackupCoreTest, CreateRejectsAnUnsupportedSectorSize)
    {
        auto info = ut::defaultInfo();
        info.sectorSize = 1024;
        info.phySectorSize = 1024;

        EXPECT_FALSE(newBackup()->create(kPath, kName, info));
    }

    TEST_F(BackupCoreTest, CreateAcceptsBothSupportedSectorSizes)
    {
        for (uint16_t sectorSize : { uint16_t(512), uint16_t(4096) }) {
            ut::MemoryRepo localRepo;
            ut::MemoryBackup backup(localRepo, manager);

            auto info = ut::defaultInfo();
            info.sectorSize = sectorSize;
            info.phySectorSize = sectorSize;

            EXPECT_TRUE(backup.create(kPath, kName, info)) << "sector size " << sectorSize;
            EXPECT_TRUE(backup.finalizeBackup());
        }
    }

    TEST_F(BackupCoreTest, CreateRejectsAPhysicalSectorSizeThatIsNotAMultiple)
    {
        auto info = ut::defaultInfo();
        info.phySectorSize = 768; // not a whole number of 512 byte sectors

        EXPECT_FALSE(newBackup()->create(kPath, kName, info));
    }

    TEST_F(BackupCoreTest, CreateRejectsAZeroPhysicalSectorSize)
    {
        auto info = ut::defaultInfo();
        info.phySectorSize = 0;

        EXPECT_FALSE(newBackup()->create(kPath, kName, info));
    }

    TEST_F(BackupCoreTest, CreateRejectsAnAlignmentBeyondTheLimit)
    {
        auto info = ut::defaultInfo();
        info.alignment = 0x100001;

        EXPECT_FALSE(newBackup()->create(kPath, kName, info));
    }

    TEST_F(BackupCoreTest, CreateAcceptsTheLargestAllowedAlignment)
    {
        auto info = ut::defaultInfo();
        info.alignment = 0x100000;

        auto backup = newBackup();
        EXPECT_TRUE(backup->create(kPath, kName, info));
        EXPECT_TRUE(backup->finalizeBackup());
    }

    TEST_F(BackupCoreTest, CreateRejectsAnUnsupportedCompressionLevel)
    {
        auto info = ut::defaultInfo();
        info.compression = 23; // one past the highest zstd level

        EXPECT_FALSE(newBackup()->create(kPath, kName, info));
    }

    TEST_F(BackupCoreTest, CreateRejectsAnUnknownEncryptionAlgorithm)
    {
        auto info = ut::defaultInfo();
        info.encryption = 2;

        EXPECT_FALSE(newBackup()->create(kPath, kName, info, "a key"));
    }

    TEST_F(BackupCoreTest, CreateRejectsEncryptionWithoutAKey)
    {
        auto info = ut::defaultInfo();
        info.encryption = 1;

        ut::CerrCapture cerr;
        EXPECT_FALSE(newBackup()->create(kPath, kName, info, ""));
        EXPECT_TRUE(cerr.contains("Encryption key is required")) << cerr.text();
    }

    TEST_F(BackupCoreTest, CreateRejectsANameTooLongForTheIndexHeader)
    {
        // The data file name is written into a fixed size list in the index
        // header. A name that does not fit used to be copied in unbounded, over
        // the index records that follow the header.
        const std::string tooLong(sizeof(Abr::IndexHeader::data_file_names), 'n');

        ut::CerrCapture cerr;
        EXPECT_FALSE(newBackup()->create(kPath, tooLong, ut::defaultInfo()));
        EXPECT_TRUE(cerr.contains("Invalid backup name")) << cerr.text();

        // The diagnostic reports the length rather than echoing a name that is
        // long enough to bury the rest of it.
        EXPECT_LT(cerr.text().size(), tooLong.size());

        // Refused before anything was created, so there is nothing to clean up.
        EXPECT_TRUE(repo.files.empty());
    }

    TEST_F(BackupCoreTest, CreateAcceptsTheLongestNameThatFits)
    {
        // One byte short of the limit once the extension and the terminator are
        // counted, so the name list ends up exactly full.
        const std::string longest(sizeof(Abr::IndexHeader::data_file_names) -
                                      strlen(Abr::DATA_FILE_EXT) - 1,
                                  'n');

        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, longest, ut::defaultInfo()));
        ASSERT_TRUE(backup->finalizeBackup());

        const auto header = readHeader(longest);
        EXPECT_STREQ(reinterpret_cast<const char *>(header.data_file_names),
                     (longest + Abr::DATA_FILE_EXT).c_str());

        // The name reaches the last byte of the list, and the record area behind
        // the header is untouched by it.
        EXPECT_EQ(repo.sizeOf(indexFile(longest)), Abr::INDEX_FILE_HEADER_SIZE);
    }

    TEST_F(BackupCoreTest, CreateRefusesToOverwriteAnExistingBackup)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));
        ASSERT_TRUE(backup->finalizeBackup());

        // The index is created without the partial write flag, so an existing one
        // must stop a second backup from taking the same name.
        EXPECT_FALSE(newBackup()->create(kPath, kName, ut::defaultInfo()));

        // ... and the backup that was already there is still intact.
        EXPECT_TRUE(repo.has(indexFile()));
        EXPECT_TRUE(repo.has(dataFile()));
    }

    TEST_F(BackupCoreTest, CreateRemovesTheIndexWhenTheDataFileCannotBeCreated)
    {
        repo.failCreateOf = dataFile();

        EXPECT_FALSE(newBackup()->create(kPath, kName, ut::defaultInfo()));

        // An index without its data file would be listed as a backup that cannot
        // be restored, so the half that was created has to be removed again.
        EXPECT_FALSE(repo.has(indexFile()));
        EXPECT_TRUE(repo.files.empty());
    }

    //
    // Writing data
    //

    TEST_F(BackupCoreTest, AnAllZeroRangeIsNotStored)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        std::vector<char> zeros(0x4000, 0);
        EXPECT_TRUE(backup->saveDataRange(zeros.data(), zeros.size(), 0));

        EXPECT_TRUE(backup->records().empty()) << "empty blocks must not get an index record";
        EXPECT_EQ(repo.sizeOf(dataFile()), 0u);

        ASSERT_TRUE(backup->finalizeBackup());
        EXPECT_EQ(readHeader().index_count, 0u);
    }

    TEST_F(BackupCoreTest, AZeroLengthRangeIsAccepted)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        char byte = 0;
        EXPECT_TRUE(backup->saveDataRange(&byte, 0, 0));
        EXPECT_TRUE(backup->records().empty());
    }

    TEST_F(BackupCoreTest, AnAllZeroRangeShorterThanABlockIsDropped)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // Shorter than the 0x1000 block size, so the empty block scan reports
        // nothing to skip and the non-empty scan nothing to store. The range
        // still has to be consumed and dropped: this used to spin forever.
        std::vector<char> zeros(0x800, 0);
        EXPECT_TRUE(backup->saveDataRange(zeros.data(), zeros.size(), 0));

        EXPECT_TRUE(backup->records().empty());
        EXPECT_EQ(repo.sizeOf(dataFile()), 0u);
    }

    TEST_F(BackupCoreTest, AnAllZeroRangeThatIsNotAWholeNumberOfBlocksIsDropped)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // One whole empty block plus an empty half block, which is the same
        // situation one iteration further in.
        std::vector<char> zeros(0x1800, 0);
        EXPECT_TRUE(backup->saveDataRange(zeros.data(), zeros.size(), 0x4000));

        EXPECT_TRUE(backup->records().empty());
    }

    TEST_F(BackupCoreTest, ATrailingPartialEmptyBlockIsDroppedNotStored)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // Data in the first block, an empty half block after it.
        auto buf = ut::patternBuffer(0x1800, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

        ASSERT_EQ(backup->records().size(), 1u);
        EXPECT_EQ(backup->records()[0].orig_length, 0x1000u);
        EXPECT_EQ(repo.sizeOf(dataFile()), 0x1000u);
    }

    TEST_F(BackupCoreTest, APartialBlockHoldingDataIsStored)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // Dropping short empty ranges must not turn into dropping short ranges:
        // a range below the block size that holds data is still a range.
        auto buf = ut::patternBuffer(0x800, 0x800);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0x2000));

        ASSERT_EQ(backup->records().size(), 1u);
        EXPECT_EQ(backup->records()[0].orig_offset, 0x2000u);
        EXPECT_EQ(backup->records()[0].orig_length, 0x800u);

        const auto *stored = repo.find(dataFile());
        ASSERT_NE(stored, nullptr);
        ASSERT_EQ(stored->size(), buf.size());
        EXPECT_EQ(memcmp(stored->data(), buf.data(), buf.size()), 0);
    }

    TEST_F(BackupCoreTest, ARangeThatOnlyStartsEmptyIsStoredWhole)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // The leading zeroes are not a whole block, so nothing is skipped and the
        // range is stored as it is, zeroes included.
        std::vector<char> buf(0x1800, 0);
        memset(buf.data() + 0x800, 'D', 0x1000);

        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

        ASSERT_EQ(backup->records().size(), 1u);
        EXPECT_EQ(backup->records()[0].orig_offset, 0u);
        EXPECT_EQ(backup->records()[0].orig_length, 0x1800u);
    }

    TEST_F(BackupCoreTest, AnIncrementalStillStoresAShortAllZeroRange)
    {
        auto info = ut::defaultInfo();
        info.incremental = 1;

        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, info));

        // Dropping an empty tail is only correct when a missing block reads back
        // as zeroes. In an incremental it reads back as unchanged, so a short
        // zeroed range has to be written out.
        std::vector<char> zeros(0x800, 0);
        ASSERT_TRUE(backup->saveDataRange(zeros.data(), zeros.size(), 0x2000));

        ASSERT_EQ(backup->records().size(), 1u);
        EXPECT_EQ(backup->records()[0].orig_length, 0x800u);
    }

    TEST_F(BackupCoreTest, OnlyTheNonEmptyBlocksAreStored)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // The block size is max(phySectorSize, 4096), so a 0x2000 buffer whose
        // first half holds data is stored as one 0x1000 block.
        auto buf = ut::patternBuffer(0x2000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0x10000));

        ASSERT_EQ(backup->records().size(), 1u);
        const auto &record = backup->records()[0];

        EXPECT_EQ(record.orig_offset, 0x10000u);
        EXPECT_EQ(record.orig_length, 0x1000u);
        EXPECT_EQ(record.chunk_offset, 0u);
        EXPECT_EQ(record.chunk_length, 0x1000u);
        EXPECT_EQ(repo.sizeOf(dataFile()), 0x1000u);
    }

    TEST_F(BackupCoreTest, LeadingEmptyBlocksAdvanceTheStoredOffset)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // First block empty, second block full: the record has to describe the
        // second block, not the start of the buffer.
        std::vector<char> buf(0x2000, 0);
        memset(buf.data() + 0x1000, 'Z', 0x1000);

        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0x20000));

        ASSERT_EQ(backup->records().size(), 1u);
        EXPECT_EQ(backup->records()[0].orig_offset, 0x21000u);
        EXPECT_EQ(backup->records()[0].orig_length, 0x1000u);
    }

    TEST_F(BackupCoreTest, AnIncrementalBackupStoresEmptyBlocksAsWell)
    {
        auto info = ut::defaultInfo();
        info.incremental = 1;

        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, info));

        // In an incremental a missing block means "unchanged", so a block that
        // was zeroed has to be written out; dropping it would leave the parent's
        // stale data in place at restore time.
        std::vector<char> zeros(0x2000, 0);
        ASSERT_TRUE(backup->saveDataRange(zeros.data(), zeros.size(), 0));

        ASSERT_EQ(backup->records().size(), 1u);
        EXPECT_EQ(backup->records()[0].orig_length, 0x2000u);
        EXPECT_EQ(repo.sizeOf(dataFile()), 0x2000u);

        ASSERT_TRUE(backup->finalizeBackup());
        EXPECT_EQ(readHeader().incremental, 1u);
    }

    TEST_F(BackupCoreTest, ChunksAreSplitAtMaxDataBlock)
    {
        auto info = ut::defaultInfo();
        info.maxDataBlock = 0x1000;

        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, info));

        auto buf = ut::patternBuffer(0x4000, 0x4000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

        ASSERT_EQ(backup->records().size(), 4u);

        for (uint32_t i = 0; i < 4; i++) {
            const auto &record = backup->records()[i];
            EXPECT_EQ(record.orig_length, 0x1000u) << "record " << i;
            EXPECT_EQ(record.orig_offset, uint64_t(i) * 0x1000) << "record " << i;
            EXPECT_EQ(record.chunk_offset, uint64_t(i) * 0x1000) << "record " << i;
        }
    }

    TEST_F(BackupCoreTest, MinDataBlockRaisesTheEmptyBlockGranularity)
    {
        auto info = ut::defaultInfo();
        info.minDataBlock = 0x4000;

        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, info));

        // Only the first 0x100 bytes hold data. With the default 0x1000 block
        // size that would store 0x1000 bytes; minDataBlock widens the block, so
        // the zeroes up to 0x4000 are stored along with it.
        auto buf = ut::patternBuffer(0x8000, 0x100);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

        ASSERT_EQ(backup->records().size(), 1u);
        EXPECT_EQ(backup->records()[0].orig_length, 0x4000u);
    }

    TEST_F(BackupCoreTest, DataIsStoredVerbatimWithoutCompressionOrEncryption)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

        const auto *stored = repo.find(dataFile());
        ASSERT_NE(stored, nullptr);
        ASSERT_EQ(stored->size(), buf.size());
        EXPECT_EQ(memcmp(stored->data(), buf.data(), buf.size()), 0);
    }

    TEST_F(BackupCoreTest, ConsecutiveChunksAreAppendedInOrder)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto first = ut::patternBuffer(0x1000, 0x1000, 'A');
        auto second = ut::patternBuffer(0x2000, 0x2000, 'N');

        ASSERT_TRUE(backup->saveDataRange(first.data(), first.size(), 0));
        ASSERT_TRUE(backup->saveDataRange(second.data(), second.size(), 0x8000));

        ASSERT_EQ(backup->records().size(), 2u);
        EXPECT_EQ(backup->records()[0].chunk_offset, 0u);
        EXPECT_EQ(backup->records()[1].chunk_offset, 0x1000u);

        const auto *stored = repo.find(dataFile());
        ASSERT_NE(stored, nullptr);
        ASSERT_EQ(stored->size(), first.size() + second.size());
        EXPECT_EQ(memcmp(stored->data(), first.data(), first.size()), 0);
        EXPECT_EQ(memcmp(stored->data() + first.size(), second.data(), second.size()), 0);
    }

    TEST_F(BackupCoreTest, SizeCountersFollowWhatWasActuallyStored)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        // Half of the buffer is dropped as empty, so only the other half counts.
        auto buf = ut::patternBuffer(0x2000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));
        ASSERT_TRUE(backup->finalizeBackup());

        const auto header = readHeader();
        EXPECT_EQ(header.orig_data_size, 0x1000u);
        EXPECT_EQ(header.stored_data_size, 0x1000u) << "raw data is stored one to one";
    }

    TEST_F(BackupCoreTest, BufferedWritesProduceTheSameDataFile)
    {
        // The S3 backend writes through a buffer of its own, the file backend
        // writes straight through. Both have to end up with identical bytes.
        auto buf = ut::patternBuffer(0x4000, 0x4000);

        ut::MemoryRepo directRepo;
        {
            ut::MemoryBackup direct(directRepo, manager);
            ASSERT_TRUE(direct.create(kPath, kName, ut::defaultInfo()));
            ASSERT_TRUE(direct.saveDataRange(buf.data(), buf.size(), 0));
            ASSERT_TRUE(direct.finalizeBackup());
        }

        ut::MemoryRepo bufferedRepo;
        {
            ut::MemoryBackup buffered(bufferedRepo, manager);
            buffered.setWriteSizes(0x1400, 0x2800); // deliberately not a chunk multiple
            ASSERT_TRUE(buffered.create(kPath, kName, ut::defaultInfo()));
            ASSERT_TRUE(buffered.saveDataRange(buf.data(), buf.size(), 0));
            ASSERT_TRUE(buffered.finalizeBackup());
        }

        EXPECT_EQ(*directRepo.find(dataFile()), *bufferedRepo.find(dataFile()));
        EXPECT_EQ(*directRepo.find(indexFile()), *bufferedRepo.find(indexFile()));
        EXPECT_EQ(directRepo.openHandles, 0);
        EXPECT_EQ(bufferedRepo.openHandles, 0);
    }

    TEST_F(BackupCoreTest, SaveFailsWhenTheDataWriteFails)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        repo.failWriteTo = dataFile();

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        EXPECT_FALSE(backup->saveDataRange(buf.data(), buf.size(), 0));
    }

    //
    // Finalizing
    //

    TEST_F(BackupCoreTest, FinalizeWritesTheIndexHeader)
    {
        auto info = ut::defaultInfo();
        info.sectors = 0x40000;
        info.minDataBlock = 0x1000;
        info.maxDataBlock = 0x8000;

        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, info));

        auto buf = ut::patternBuffer(0x2000, 0x2000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));
        ASSERT_TRUE(backup->finalizeBackup());

        const auto header = readHeader();

        EXPECT_EQ(memcmp(header.signature, Abr::INDEX_FILE_SIGNATURE, sizeof(header.signature)), 0);
        EXPECT_EQ(header.version, Abr::INDEX_FILE_VERSION);
        EXPECT_EQ(header.sector_size, info.sectorSize);
        EXPECT_EQ(header.phy_sector_size, info.phySectorSize);
        EXPECT_EQ(header.image_size, uint64_t(info.sectors) * info.sectorSize);
        EXPECT_EQ(header.min_data_block, info.minDataBlock);
        EXPECT_EQ(header.max_data_block, info.maxDataBlock);
        EXPECT_EQ(header.compression, 0u);
        EXPECT_EQ(header.encryption, 0u);
        EXPECT_EQ(header.incremental, 0u);
        EXPECT_EQ(header.index_count, 1u);
        EXPECT_EQ(header.index_offset, Abr::INDEX_FILE_HEADER_SIZE);
        EXPECT_EQ(header.data_file_names_offset, offsetof(Abr::IndexHeader, data_file_names));

        // The data file is named in the header so a restore can find it without
        // reconstructing the name.
        EXPECT_STREQ(reinterpret_cast<const char *>(header.data_file_names),
                     (std::string(kName) + Abr::DATA_FILE_EXT).c_str());
    }

    TEST_F(BackupCoreTest, TheIndexFileIsAHeaderFollowedByItsRecords)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        for (int i = 0; i < 3; i++) {
            ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), uint64_t(i) * 0x10000));
        }
        ASSERT_TRUE(backup->finalizeBackup());

        EXPECT_EQ(repo.sizeOf(indexFile()), Abr::INDEX_FILE_HEADER_SIZE + 3 * Abr::INDEX_FILE_RECORD_SIZE);
        EXPECT_EQ(readHeader().index_count, 3u);
        EXPECT_EQ(readRecords().size(), 3u);
    }

    TEST_F(BackupCoreTest, TheHeaderChecksumCoversTheHeader)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));
        ASSERT_TRUE(backup->finalizeBackup());

        auto header = readHeader();
        const uint32_t stored = header.header_crc32c;

        // Recomputed the way a reader does it: over the whole header with the
        // checksum field zeroed.
        header.header_crc32c = 0;
        const uint32_t computed =
            crc32c::Crc32c(reinterpret_cast<const uint8_t *>(&header), sizeof(header));

        EXPECT_NE(stored, 0u);
        EXPECT_EQ(stored, computed);
    }

    TEST_F(BackupCoreTest, TheIndexChecksumCoversTheRecords)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0x10000));
        ASSERT_TRUE(backup->finalizeBackup());

        const auto records = readRecords();
        ASSERT_EQ(records.size(), 2u);

        const uint32_t computed = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(records.data()),
                                                 records.size() * sizeof(Abr::IndexRecord));

        EXPECT_EQ(readHeader().index_crc32c, computed);
    }

    TEST_F(BackupCoreTest, EachChunkChecksumCoversTheStoredChunk)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto first = ut::patternBuffer(0x1000, 0x1000, 'A');
        auto second = ut::patternBuffer(0x1000, 0x1000, 'M');
        ASSERT_TRUE(backup->saveDataRange(first.data(), first.size(), 0));
        ASSERT_TRUE(backup->saveDataRange(second.data(), second.size(), 0x10000));
        ASSERT_TRUE(backup->finalizeBackup());

        const auto records = readRecords();
        const auto *data = repo.find(dataFile());
        ASSERT_EQ(records.size(), 2u);
        ASSERT_NE(data, nullptr);

        for (size_t i = 0; i < records.size(); i++) {
            const auto &record = records[i];
            ASSERT_LE(record.chunk_offset + record.chunk_length, data->size());

            const uint32_t computed = crc32c::Crc32c(
                reinterpret_cast<const uint8_t *>(data->data() + record.chunk_offset),
                record.chunk_length);

            EXPECT_NE(record.chunk_crc32c, 0u) << "record " << i;
            EXPECT_EQ(record.chunk_crc32c, computed) << "record " << i;
        }
    }

    TEST_F(BackupCoreTest, ABackupWithoutDataIsStillValid)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));
        ASSERT_TRUE(backup->finalizeBackup());

        EXPECT_EQ(repo.sizeOf(indexFile()), Abr::INDEX_FILE_HEADER_SIZE);
        EXPECT_TRUE(repo.has(dataFile()));
        EXPECT_EQ(repo.sizeOf(dataFile()), 0u);
        EXPECT_EQ(readHeader().index_count, 0u);
    }

    TEST_F(BackupCoreTest, FinalizingTwiceFails)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));
        ASSERT_TRUE(backup->finalizeBackup());

        ut::CerrCapture cerr;
        EXPECT_FALSE(backup->finalizeBackup());
        EXPECT_TRUE(cerr.contains("is not open")) << cerr.text();

        // The committed backup is untouched by the refused second attempt.
        EXPECT_TRUE(repo.has(indexFile()));
        EXPECT_TRUE(repo.has(dataFile()));
    }

    TEST_F(BackupCoreTest, FinalizingWithoutCreatingFails)
    {
        EXPECT_FALSE(newBackup()->finalizeBackup());
    }

    TEST_F(BackupCoreTest, AbortRemovesBothFiles)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

        EXPECT_TRUE(backup->finalizeBackup(true));
        EXPECT_TRUE(repo.files.empty()) << "an aborted backup must leave nothing behind";
    }

    TEST_F(BackupCoreTest, AbortingACommittedBackupDoesNotRemoveIt)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));
        ASSERT_TRUE(backup->finalizeBackup());

        // Once committed the files are a repository object of their own; a later
        // abort of the same object must not reach them.
        EXPECT_TRUE(backup->finalizeBackup(true));
        EXPECT_TRUE(repo.has(indexFile()));
        EXPECT_TRUE(repo.has(dataFile()));
    }

    TEST_F(BackupCoreTest, DroppingAnUnfinalizedBackupRemovesItsFiles)
    {
        {
            auto backup = newBackup();
            ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

            auto buf = ut::patternBuffer(0x1000, 0x1000);
            ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

            EXPECT_TRUE(repo.has(indexFile()));
        }

        // Until finalizeBackup() has written the index, what was written is
        // partial data that must not outlive the object that wrote it.
        EXPECT_TRUE(repo.files.empty());
    }

    TEST_F(BackupCoreTest, AFailedIndexWriteDiscardsTheBackup)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));

        repo.failWriteTo = indexFile();

        EXPECT_FALSE(backup->finalizeBackup());
        EXPECT_TRUE(repo.files.empty()) << "a backup that could not be committed must be removed";
    }

    TEST_F(BackupCoreTest, AFailedDiscardIsReported)
    {
        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, ut::defaultInfo()));

        repo.failDeleteOf = indexFile();

        ut::CerrCapture cerr;
        EXPECT_FALSE(backup->finalizeBackup(true));

        // The caller has to learn that something of the backup is still there.
        EXPECT_TRUE(cerr.contains("has to be deleted manually")) << cerr.text();

        repo.failDeleteOf.clear();
    }

    TEST_F(BackupCoreTest, CreateStartsTheSizeCountersAtZero)
    {
        // A caller may hand back a BackupInfo it got from getInfo(), which still
        // carries the totals of the backup it came from. Those must not be
        // counted into the new one.
        auto info = ut::defaultInfo();
        info.orig_data_size = 999;
        info.stored_data_size = 777;

        auto backup = newBackup();
        ASSERT_TRUE(backup->create(kPath, kName, info));

        auto buf = ut::patternBuffer(0x1000, 0x1000);
        ASSERT_TRUE(backup->saveDataRange(buf.data(), buf.size(), 0));
        ASSERT_TRUE(backup->finalizeBackup());

        const auto header = readHeader();
        EXPECT_EQ(header.orig_data_size, 0x1000u);
        EXPECT_EQ(header.stored_data_size, 0x1000u);
    }

    //
    // Block scanning
    //

    class BlockScanTest : public testing::Test
    {
    protected:
        ut::MemoryRepo repo;
        ut::MemoryBackup backup{ repo, ut::stubManager() };
    };

    TEST_F(BlockScanTest, EmptyScanCountsOnlyWholeBlocks)
    {
        std::vector<uint8_t> buf(0x3000, 0);

        EXPECT_EQ(backup.findEmptyBlockLength(buf.data(), buf.size(), 0x1000), 0x3000u);
    }

    TEST_F(BlockScanTest, EmptyScanStopsAtTheFirstBlockWithData)
    {
        std::vector<uint8_t> buf(0x4000, 0);
        buf[0x2000] = 1;

        EXPECT_EQ(backup.findEmptyBlockLength(buf.data(), buf.size(), 0x1000), 0x2000u);
    }

    TEST_F(BlockScanTest, EmptyScanIgnoresATrailingPartialBlock)
    {
        // A partial block is never reported as skippable, so a range that is not
        // a whole number of blocks keeps its tail.
        std::vector<uint8_t> buf(0x1800, 0);

        EXPECT_EQ(backup.findEmptyBlockLength(buf.data(), buf.size(), 0x1000), 0x1000u);
    }

    TEST_F(BlockScanTest, EmptyScanReturnsZeroWhenTheFirstBlockHasData)
    {
        std::vector<uint8_t> buf(0x2000, 0);
        buf[0x0fff] = 1;

        EXPECT_EQ(backup.findEmptyBlockLength(buf.data(), buf.size(), 0x1000), 0u);
    }

    TEST_F(BlockScanTest, NonEmptyScanStopsAtTheFirstEmptyBlock)
    {
        std::vector<uint8_t> buf(0x4000, 0xAB);
        memset(buf.data() + 0x2000, 0, 0x1000);

        EXPECT_EQ(backup.findNonEmptyBlockLength(buf.data(), buf.size(), 0x1000), 0x2000u);
    }

    TEST_F(BlockScanTest, NonEmptyScanClampsToTheSuppliedLength)
    {
        // The final partial block holds data, so it is included, and the result
        // never exceeds the length the caller passed in.
        std::vector<uint8_t> buf(0x1800, 0xCD);

        EXPECT_EQ(backup.findNonEmptyBlockLength(buf.data(), buf.size(), 0x1000), 0x1800u);
    }

    //
    // saveDataRange() consumes findEmptyBlockLength() + findNonEmptyBlockLength()
    // bytes per iteration, and gives up the rest of the range when the pair
    // reports nothing - which happens for an all-zero remainder shorter than one
    // block, since the empty scan counts whole blocks only. Without that the loop
    // would spin, appending an index record on every pass.
    //
    // This test walks a range the way saveDataRange() does, with a guard, so a
    // regression is reported instead of hanging the suite.
    //
    TEST_F(BlockScanTest, TheScanPairEitherMakesProgressOrLeavesOnlyEmptyBytes)
    {
        constexpr uint32_t blockSize = 0x1000;

        for (uint32_t total : { 0x800u, 0x1800u, 0x2800u }) {
            std::vector<uint8_t> buf(total, 0);

            uint8_t *ptr = buf.data();
            uint32_t remaining = total;

            for (int guard = 0; remaining && guard < 16; guard++) {
                const uint32_t empty = backup.findEmptyBlockLength(ptr, remaining, blockSize);
                ptr += empty;
                remaining -= empty;

                if (!remaining) {
                    break;
                }

                const uint32_t nonEmpty = backup.findNonEmptyBlockLength(ptr, remaining, blockSize);

                if (nonEmpty == 0) {
                    // saveDataRange() stops here, so everything left has to be
                    // empty; anything else would be dropped from the backup.
                    EXPECT_EQ(ut::firstNonZero(ptr, remaining), -1)
                        << "saveDataRange() would drop " << remaining << " bytes that hold data";
                    remaining = 0;
                    break;
                }

                ptr += nonEmpty;
                remaining -= nonEmpty;
            }

            EXPECT_EQ(remaining, 0u) << "an all-zero range of " << total << " was not consumed";
        }
    }

    TEST_F(BlockScanTest, NonEmptyScanReturnsZeroForALeadingEmptyBlock)
    {
        std::vector<uint8_t> buf(0x2000, 0);
        memset(buf.data() + 0x1000, 0xEF, 0x1000);

        EXPECT_EQ(backup.findNonEmptyBlockLength(buf.data(), buf.size(), 0x1000), 0u);
    }
}
