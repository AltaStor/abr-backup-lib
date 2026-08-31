// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Shared fixture for the backup/restore core tests
//

#pragma once

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crc32c/crc32c.h"

#include "memory_backend.h"
#include "test_support.h"

namespace ut
{
    //
    // A test that drives BackupCore and RestoreCore against one in-memory
    // repository: writeBackup() puts a committed backup into it, the reader
    // helpers hand back what was stored, and the patch helpers damage it.
    //
    class CoreTest : public testing::Test
    {
    protected:
        static constexpr const char *kPath = "vm-01";
        static constexpr const char *kName = "daily";

        // One extent of original data: where it belongs in the image, and its bytes.
        using NamedRange = std::pair<uint64_t, std::vector<char>>;

        void TearDown() override
        {
            // Every backup and restore object is gone by now, so anything still
            // checked out is a handle the core failed to release.
            EXPECT_EQ(repo.openHandles, 0) << "a file handle was left open";
        }

        std::string indexFile(const std::string &name = kName) const
        {
            return ut::indexName(kPath, name);
        }

        std::string dataFile(const std::string &name = kName) const
        {
            return ut::dataName(kPath, name);
        }

        // Writes and commits a backup made of the given ranges.
        void writeBackup(const Abr::BackupInfo &info, const std::vector<NamedRange> &ranges,
                         const std::string &encryptionKey = "", const std::string &name = kName)
        {
            ut::MemoryBackup backup(repo, manager);

            ASSERT_TRUE(backup.create(kPath, name, info, encryptionKey));

            for (const auto &range : ranges) {
                auto bytes = range.second;
                ASSERT_TRUE(backup.saveDataRange(bytes.data(), static_cast<uint32_t>(bytes.size()),
                                                 range.first));
            }

            ASSERT_TRUE(backup.finalizeBackup());
        }

        std::unique_ptr<ut::MemoryRestore> newRestore()
        {
            return std::make_unique<ut::MemoryRestore>(repo, manager);
        }

        Abr::IndexHeader readHeader(const std::string &name = kName) const
        {
            Abr::IndexHeader header{};

            auto it = repo.files.find(ut::indexName(kPath, name));
            EXPECT_NE(it, repo.files.end()) << "no index file was written";
            if (it != repo.files.end() && it->second.size() >= sizeof(header)) {
                memcpy(&header, it->second.data(), sizeof(header));
            }

            return header;
        }

        std::vector<Abr::IndexRecord> readRecords(const std::string &name = kName) const
        {
            std::vector<Abr::IndexRecord> records;

            auto it = repo.files.find(ut::indexName(kPath, name));
            if (it == repo.files.end() || it->second.size() < Abr::INDEX_FILE_HEADER_SIZE) {
                return records;
            }

            const size_t recordBytes = it->second.size() - Abr::INDEX_FILE_HEADER_SIZE;
            records.resize(recordBytes / Abr::INDEX_FILE_RECORD_SIZE);

            if (!records.empty()) {
                memcpy(records.data(), it->second.data() + Abr::INDEX_FILE_HEADER_SIZE,
                       records.size() * sizeof(Abr::IndexRecord));
            }

            return records;
        }

        // Overwrites a field of the stored index header. The header checksum is
        // left as it was, so use resealHeader() when the change is meant to be
        // accepted rather than caught.
        template <typename T>
        void patchHeader(size_t offset, T value, const std::string &name = kName)
        {
            auto *data = repo.find(ut::indexName(kPath, name));
            ASSERT_NE(data, nullptr);
            ASSERT_GE(data->size(), offset + sizeof(T));

            memcpy(data->data() + offset, &value, sizeof(T));
        }

        // Recomputes the header checksum so a patched header reads as intact and
        // the test reaches the validation it is actually aiming at.
        void resealHeader(const std::string &name = kName)
        {
            auto *data = repo.find(ut::indexName(kPath, name));
            ASSERT_NE(data, nullptr);
            ASSERT_GE(data->size(), sizeof(Abr::IndexHeader));

            Abr::IndexHeader header{};
            memcpy(&header, data->data(), sizeof(header));

            header.header_crc32c = 0;
            header.header_crc32c =
                crc32c::Crc32c(reinterpret_cast<const uint8_t *>(&header), sizeof(header));

            memcpy(data->data(), &header, sizeof(header));
        }

        // Repairs every checksum covering a chunk that a test has altered: the
        // chunk's own, the index checksum over the records, and the header
        // checksum. A backup resealed this way is indistinguishable from an
        // intact one to everything except the encryption tag.
        void resealChunk(uint32_t recordIndex, const std::string &name = kName)
        {
            auto *index = repo.find(ut::indexName(kPath, name));
            auto *data = repo.find(ut::dataName(kPath, name));
            ASSERT_NE(index, nullptr);
            ASSERT_NE(data, nullptr);

            auto records = readRecords(name);
            ASSERT_LT(recordIndex, records.size());

            auto &record = records[recordIndex];
            ASSERT_LE(record.chunk_offset + record.chunk_length, data->size());

            record.chunk_crc32c = crc32c::Crc32c(
                reinterpret_cast<const uint8_t *>(data->data() + record.chunk_offset),
                record.chunk_length);

            const size_t recordBytes = records.size() * sizeof(Abr::IndexRecord);
            memcpy(index->data() + Abr::INDEX_FILE_HEADER_SIZE, records.data(), recordBytes);

            Abr::IndexHeader header{};
            memcpy(&header, index->data(), sizeof(header));
            header.index_crc32c =
                crc32c::Crc32c(reinterpret_cast<const uint8_t *>(records.data()), recordBytes);
            header.header_crc32c = 0;
            header.header_crc32c =
                crc32c::Crc32c(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
            memcpy(index->data(), &header, sizeof(header));
        }

        MemoryRepo repo;
        std::shared_ptr<Abr::IBackupManager> manager = stubManager();
    };
}
