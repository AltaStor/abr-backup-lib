// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// An in-memory repository for the backup/restore core tests.
//
// BackupCore and RestoreCore hold all of the format logic and delegate every
// byte that reaches storage to four virtual file operations. Implementing those
// against a std::map lets the tests exercise the format, the empty block
// elimination, the CRCs and the crypto without a filesystem or a network, and
// gives them two things a real backend cannot offer: the bytes of the written
// files, so the on-disk layout can be asserted directly, and injected failures
// on any single operation, so the error paths are reachable.
//

#pragma once

#include "backup_core.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ut
{
    //
    // The stored files, keyed by the name the core passes to the file
    // operations, which is "<path>/<name><ext>" relative to the repository.
    //
    struct MemoryRepo
    {
        std::map<std::string, std::vector<char>> files;

        // Handles currently checked out. A backup or restore that is done with
        // the repository has to leave this at zero.
        int openHandles = 0;

        // Failure injection. Each names the file whose operation must fail, so a
        // test can break exactly one step of a sequence.
        std::string failCreateOf;
        std::string failOpenOf;
        std::string failWriteTo;
        std::string failDeleteOf;

        // When >= 0, the write with this 0-based number fails, whichever file it
        // targets. Used to break a write in the middle of a chunk sequence.
        int failWriteAfter = -1;

        int writes = 0;
        int reads = 0;

        bool has(const std::string &name) const
        {
            return files.find(name) != files.end();
        }

        std::vector<char> *find(const std::string &name)
        {
            auto it = files.find(name);
            return it == files.end() ? nullptr : &it->second;
        }

        size_t sizeOf(const std::string &name)
        {
            auto *data = find(name);
            return data ? data->size() : 0;
        }

        // Flips the bits of one byte, which is the cheapest way to make a stored
        // CRC disagree with the data it covers.
        bool corrupt(const std::string &name, size_t offset)
        {
            auto *data = find(name);
            if (!data || offset >= data->size()) {
                return false;
            }
            (*data)[offset] = static_cast<char>(~(*data)[offset]);
            return true;
        }
    };

    struct MemoryHandle
    {
        std::string name;
        MemoryRepo *repo;
    };

    //
    // The core keeps a manager pointer but never dereferences it, so the tests
    // only need something that satisfies the interface.
    //
    class StubManager : public Abr::IBackupManager
    {
    public:
        explicit StubManager(std::string folder = "")
        {
            m_folder = std::move(folder);
        }

        bool initialize(const Abr::BackupManagerParams &) override
        {
            return true;
        }

        bool listBackups(const std::string &, std::vector<std::string> &) override
        {
            return false;
        }

        bool createBackup(const std::string &, const std::string &, const Abr::BackupInfo &,
                          std::shared_ptr<Abr::IBackup> &, const std::string) override
        {
            return false;
        }

        bool openBackup(const std::string &, const std::string &,
                        std::shared_ptr<Abr::IRestore> &, const std::string) override
        {
            return false;
        }

        bool loadMetaData(const std::string &, const std::string &, std::ostream &) override
        {
            return false;
        }

        bool saveMetaData(const std::string &, const std::string &, std::istream &) override
        {
            return false;
        }

        bool deleteBackup(const std::string &, const std::string &) override
        {
            return false;
        }
    };

    inline std::shared_ptr<Abr::IBackupManager> stubManager()
    {
        return std::make_shared<StubManager>();
    }

    //
    // A backup that writes into a MemoryRepo.
    //
    class MemoryBackup : public Abr::BackupCore
    {
    public:
        MemoryBackup(MemoryRepo &repo, std::shared_ptr<Abr::IBackupManager> manager)
            : Abr::BackupCore(std::move(manager))
            , m_repo(repo)
        {}

        ~MemoryBackup() override
        {
            // Required of every BackupCore subclass: the base destructor asserts
            // that nothing of an unfinalized backup is left behind, and only a
            // derived destructor still has the file operations to remove it.
            discardPartialBackup();
        }

        // The scan helpers and the discard are protected in the core; the tests
        // drive them directly.
        using Abr::BackupCore::discardPartialBackup;
        using Abr::BackupCore::findEmptyBlockLength;
        using Abr::BackupCore::findNonEmptyBlockLength;

        // The write sizes decide between the direct write path used by the file
        // backend and the buffered path used by the S3 backend.
        void setWriteSizes(uint32_t minWriteSize, uint32_t maxWriteSize)
        {
            m_minWriteSize = minWriteSize;
            m_maxWriteSize = maxWriteSize;
        }

        const std::vector<Abr::IndexRecord> &records() const
        {
            return m_indexRecords;
        }

    protected:
        Handle createFile(const std::string &fullName, bool partialWrite) override
        {
            if (!partialWrite && m_repo.has(fullName)) {
                std::cerr << "Error: File '" << fullName << "' already exists!" << std::endl;
                return nullptr;
            }

            if (!m_repo.failCreateOf.empty() && m_repo.failCreateOf == fullName) {
                std::cerr << "Injected create failure for '" << fullName << "'" << std::endl;
                return nullptr;
            }

            m_repo.files[fullName].clear();
            m_repo.openHandles++;

            return new MemoryHandle{ fullName, &m_repo };
        }

        bool writeFile(Handle hFile, void *pDataBuf, uint32_t dataLength) override
        {
            auto *handle = static_cast<MemoryHandle *>(hFile);

            const bool injected = (!m_repo.failWriteTo.empty() && m_repo.failWriteTo == handle->name) ||
                                  (m_repo.failWriteAfter >= 0 && m_repo.writes == m_repo.failWriteAfter);

            m_repo.writes++;

            if (injected) {
                std::cerr << "Injected write failure for '" << handle->name << "'" << std::endl;
                return false;
            }

            auto &data = m_repo.files[handle->name];
            const auto *bytes = static_cast<const char *>(pDataBuf);
            data.insert(data.end(), bytes, bytes + dataLength);

            return true;
        }

        bool closeFile(Handle hFile, bool bDelete = false) override
        {
            auto *handle = static_cast<MemoryHandle *>(hFile);
            if (!handle) {
                return true;
            }

            m_repo.openHandles--;
            const std::string name = handle->name;
            delete handle;

            return bDelete ? deleteFile(name) : true;
        }

        bool deleteFile(const std::string &fullName) override
        {
            if (!m_repo.failDeleteOf.empty() && m_repo.failDeleteOf == fullName) {
                std::cerr << "Injected delete failure for '" << fullName << "'" << std::endl;
                return false;
            }

            // A file that is not there counts as removed, as in the file backend.
            m_repo.files.erase(fullName);
            return true;
        }

        MemoryRepo &m_repo;
    };

    //
    // A restore that reads from a MemoryRepo.
    //
    class MemoryRestore : public Abr::RestoreCore
    {
    public:
        MemoryRestore(MemoryRepo &repo, std::shared_ptr<Abr::IBackupManager> manager)
            : Abr::RestoreCore(std::move(manager))
            , m_repo(repo)
        {}

        ~MemoryRestore() override
        {
            if (m_hDataFile != nullptr) {
                close();
            }
        }

        uint32_t indexRecordCount() const
        {
            return m_numIndexRecords;
        }

        const Abr::IndexRecord *indexRecords() const
        {
            return m_pIndexRecords;
        }

    protected:
        Handle openFile(const std::string &fullName) override
        {
            if (!m_repo.failOpenOf.empty() && m_repo.failOpenOf == fullName) {
                std::cerr << "Injected open failure for '" << fullName << "'" << std::endl;
                return nullptr;
            }

            if (!m_repo.has(fullName)) {
                std::cerr << "Failed to open file '" << fullName << "'!" << std::endl;
                return nullptr;
            }

            m_repo.openHandles++;

            return new MemoryHandle{ fullName, &m_repo };
        }

        bool readFile(Handle hFile, void *pDataBuf, uint64_t offset, uint32_t dataLength) override
        {
            auto *handle = static_cast<MemoryHandle *>(hFile);
            auto *data = m_repo.find(handle->name);

            m_repo.reads++;

            if (!data || offset + dataLength > data->size()) {
                std::cerr << "Error: Attempting to read beyond the end of the file!" << std::endl;
                return false;
            }

            memcpy(pDataBuf, data->data() + offset, dataLength);
            return true;
        }

        bool closeFile(Handle hFile) override
        {
            auto *handle = static_cast<MemoryHandle *>(hFile);
            if (!handle) {
                return true;
            }

            m_repo.openHandles--;
            delete handle;

            return true;
        }

        bool deleteFile(const std::string &fullName) override
        {
            if (!m_repo.failDeleteOf.empty() && m_repo.failDeleteOf == fullName) {
                std::cerr << "Injected delete failure for '" << fullName << "'" << std::endl;
                return false;
            }

            // A restore deletes what it has opened, so a missing file is a
            // failure here, matching the file backend.
            if (!m_repo.has(fullName)) {
                return false;
            }

            m_repo.files.erase(fullName);
            return true;
        }

        MemoryRepo &m_repo;
    };

    //
    // A BackupInfo describing a plain 512-byte-sector image, which is what most
    // of the tests back up.
    //
    inline Abr::BackupInfo defaultInfo()
    {
        Abr::BackupInfo info = {};

        info.sectors = 0x10000;
        info.sectorSize = 512;
        info.phySectorSize = 512;

        return info;
    }

    inline std::string indexName(const std::string &path, const std::string &name)
    {
        return join_backup_path(path, name) + Abr::INDEX_FILE_EXT;
    }

    inline std::string dataName(const std::string &path, const std::string &name)
    {
        return join_backup_path(path, name) + Abr::DATA_FILE_EXT;
    }
}
