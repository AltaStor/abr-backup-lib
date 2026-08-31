// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor backup index file format
//
#pragma once

#include <stdint.h>

namespace Abr
{
    const char *const INDEX_FILE_EXT = ".abri";
    const char *const DATA_FILE_EXT = ".abrd";
    const char *const META_FILE_EXT = ".abrm";

    const char *const INDEX_FILE_SIGNATURE = "abrindex";
    const uint32_t INDEX_FILE_VERSION = 0x00000001;
    const uint32_t INDEX_FILE_HEADER_SIZE = 4096;
    const uint32_t INDEX_FILE_RECORD_SIZE = 32;
    const uint32_t INDEX_FILE_MAX_FILES = 256;
    
    struct IndexRecord
    {
        IndexRecord() 
        {}
        
        IndexRecord(uint64_t offset, uint32_t length)
            : orig_offset(offset)
            , orig_length(length)
        {}

        uint64_t chunk_offset; // offset of the chunk in data file

        uint32_t chunk_length; // length of the chunk in data file

        uint32_t chunk_crc32c = 0; // crc32c of the chunk in data file

        uint64_t orig_offset; // offset of the data in the original image

        uint32_t orig_length; // length of the data in the original image

        uint8_t file_id = 0; // data file index

        uint8_t reserved[3]; // must be zeroed
    };

    struct IndexHeader {
        uint8_t signature[8]; // "abrindex"

        uint16_t version; // 0x00000001

        uint8_t compression; // 0 - raw, 1-9 - Zstd compression level

        uint8_t encryption; // 0 - none, 1 - AES-256-GCM encryption

        uint32_t header_crc32c; // counted for whole header with zeroed crc32c field

        uint64_t image_size; // total size of the original image in bytes

        uint16_t sector_size; // sector size of the original image

        uint16_t phy_sector_size; // physical sector size of the original image

        uint32_t index_count; // number of index records

        uint32_t index_offset; // offset of index data from the beginning of the header

        uint32_t index_crc32c; // counted for all index records

        uint32_t min_data_block; // min data block size

        uint32_t max_data_block; // max data block size

        uint32_t data_file_names_offset; // offset of data file names from the beginning of the header

        uint8_t incremental : 1; // 0 - full, 1 - incremental (changed blocks only)
        uint8_t reserved0 : 7; // must be zeroed

        uint8_t reserved[11]; // must be zeroed

        uint8_t encryption_key[32]; // encrypted key for data blocks (if encryption is used)

        uint8_t encryption_iv[16]; // random initial vector for encryption

        uint64_t orig_data_size; // total size of the original data in bytes

        uint64_t stored_data_size; // total size of the stored and optionally compressed data in bytes

        uint8_t reserved1[448]; // must be zeroed

        uint8_t data_file_names[3520]; // zero terminated list of data file names (UFT-8)
    };
}