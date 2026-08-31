// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for the on-disk index format
//
// IndexHeader and IndexRecord are written to storage byte for byte and read back
// by every other version of this library, so their size, their field offsets and
// the constants describing them are part of the format, not implementation
// detail. These tests fail the moment a field is added, moved or resized.
//

#include <gtest/gtest.h>

#include <cstddef>

#include "abr_backup_format.h"

using Abr::IndexHeader;
using Abr::IndexRecord;

TEST(IndexFormat, HeaderHasTheDeclaredSize)
{
    EXPECT_EQ(sizeof(IndexHeader), Abr::INDEX_FILE_HEADER_SIZE);
    EXPECT_EQ(sizeof(IndexHeader), 4096u);
}

TEST(IndexFormat, RecordHasTheDeclaredSize)
{
    // finalizeBackup() writes the records as a flat array and sizes the index by
    // INDEX_FILE_RECORD_SIZE, so a mismatch would produce an unreadable index.
    EXPECT_EQ(sizeof(IndexRecord), Abr::INDEX_FILE_RECORD_SIZE);
    EXPECT_EQ(sizeof(IndexRecord), 32u);
}

TEST(IndexFormat, HeaderFieldsAreAtTheirFormatOffsets)
{
    EXPECT_EQ(offsetof(IndexHeader, signature), 0u);
    EXPECT_EQ(offsetof(IndexHeader, version), 8u);
    EXPECT_EQ(offsetof(IndexHeader, compression), 10u);
    EXPECT_EQ(offsetof(IndexHeader, encryption), 11u);
    EXPECT_EQ(offsetof(IndexHeader, header_crc32c), 12u);
    EXPECT_EQ(offsetof(IndexHeader, image_size), 16u);
    EXPECT_EQ(offsetof(IndexHeader, sector_size), 24u);
    EXPECT_EQ(offsetof(IndexHeader, phy_sector_size), 26u);
    EXPECT_EQ(offsetof(IndexHeader, index_count), 28u);
    EXPECT_EQ(offsetof(IndexHeader, index_offset), 32u);
    EXPECT_EQ(offsetof(IndexHeader, index_crc32c), 36u);
    EXPECT_EQ(offsetof(IndexHeader, min_data_block), 40u);
    EXPECT_EQ(offsetof(IndexHeader, max_data_block), 44u);
    EXPECT_EQ(offsetof(IndexHeader, data_file_names_offset), 48u);
    EXPECT_EQ(offsetof(IndexHeader, encryption_key), 64u);
    EXPECT_EQ(offsetof(IndexHeader, encryption_iv), 96u);
    EXPECT_EQ(offsetof(IndexHeader, orig_data_size), 112u);
    EXPECT_EQ(offsetof(IndexHeader, stored_data_size), 120u);
    EXPECT_EQ(offsetof(IndexHeader, data_file_names), 576u);
}

TEST(IndexFormat, RecordFieldsAreAtTheirFormatOffsets)
{
    EXPECT_EQ(offsetof(IndexRecord, chunk_offset), 0u);
    EXPECT_EQ(offsetof(IndexRecord, chunk_length), 8u);
    EXPECT_EQ(offsetof(IndexRecord, chunk_crc32c), 12u);
    EXPECT_EQ(offsetof(IndexRecord, orig_offset), 16u);
    EXPECT_EQ(offsetof(IndexRecord, orig_length), 24u);
    EXPECT_EQ(offsetof(IndexRecord, file_id), 28u);
}

TEST(IndexFormat, DataFileNamesFillTheRestOfTheHeader)
{
    // The name list is written with strcpy() into the tail of the header, so its
    // size has to be exactly what is left after the fixed fields.
    EXPECT_EQ(offsetof(IndexHeader, data_file_names) + sizeof(IndexHeader{}.data_file_names),
              Abr::INDEX_FILE_HEADER_SIZE);
    EXPECT_EQ(sizeof(IndexHeader{}.data_file_names), 3520u);
}

TEST(IndexFormat, ConstantsAreTheDocumentedOnes)
{
    EXPECT_STREQ(Abr::INDEX_FILE_SIGNATURE, "abrindex");
    EXPECT_STREQ(Abr::INDEX_FILE_EXT, ".abri");
    EXPECT_STREQ(Abr::DATA_FILE_EXT, ".abrd");
    EXPECT_STREQ(Abr::META_FILE_EXT, ".abrm");
    EXPECT_EQ(Abr::INDEX_FILE_VERSION, 1u);
    EXPECT_EQ(Abr::INDEX_FILE_MAX_FILES, 256u);
}

TEST(IndexFormat, RecordConstructorSetsTheOriginalExtent)
{
    const IndexRecord record(0x4000, 0x1000);

    EXPECT_EQ(record.orig_offset, 0x4000u);
    EXPECT_EQ(record.orig_length, 0x1000u);

    // The chunk position is filled in by the writer, only the checksum and the
    // file id have to start out defined.
    EXPECT_EQ(record.chunk_crc32c, 0u);
    EXPECT_EQ(record.file_id, 0u);
}
