// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor S3 backup and restore file definitions
//

#pragma once

#include "abr_backup.h"
#include "abr_backup_format.h"

typedef void* Handle;

// Joins a repository sub-path and a backup name into "path/name", or into just
// "name" when the sub-path is empty.
std::string join_backup_path(const std::string &path, const std::string &name);

namespace Abr
{
    //
    // A backup is only complete once finalizeBackup() has written the index; until
    // then everything saveDataRange() has written is partial data that must never
    // outlive the run that wrote it. Every path that gives up on a backup - a
    // failed create(), a failed or aborted finalizeBackup(), or simply dropping
    // the object - goes through discardPartialBackup(), which removes what has
    // been written so far.
    //
    class BackupCore : public Abr::IBackup
    {
    public:
        BackupCore(std::shared_ptr<IBackupManager> pManager)
            : m_pManager(pManager)
        {}

        virtual ~BackupCore();

        virtual bool create(const std::string &path, const std::string &name,
             const Abr::BackupInfo& info, const std::string encryptionKey = "");

        const BackupInfo& getInfo() override {
            return m_info;
        }

        bool saveDataRange(void *pDataBuf, uint32_t dataLength, uint64_t origOffset) override;

        bool finalizeBackup(bool abort = false) override;

    protected:
        // Removes everything this backup has put into the repository and leaves
        // the object in the state it had before create(), so that a discarded
        // backup can neither be finalized nor discarded twice. Does nothing once
        // finalizeBackup() has committed the backup.
        //
        // Derived classes must call this from their destructor: the base
        // destructor cannot, because the virtual file operations it needs are
        // already gone by then.
        bool discardPartialBackup();

        virtual Handle createFile(const std::string &fullName, bool partialWrite) = 0;

        virtual bool writeFile(Handle hFile, void *pDataBuf, uint32_t dataLength) = 0;

        virtual bool closeFile(Handle hFile, bool bDelete = false) = 0;

        // Removes an already closed file. A file that is not there counts as
        // removed, so discarding a backup that never got as far as creating both
        // files is not an error.
        virtual bool deleteFile(const std::string &fullName) = 0;

        uint32_t findEmptyBlockLength(uint8_t *buf, uint32_t length, uint32_t blockSize);
        uint32_t findNonEmptyBlockLength(uint8_t *buf, uint32_t length, uint32_t blockSize);

        int encryptData(const void *input, int inlen, void *output, int outlen, uint64_t offset);

    protected:
        std::shared_ptr<IBackupManager> m_pManager;
        BackupInfo m_info;

        uint32_t m_minWriteSize = 0;
        uint32_t m_maxWriteSize = 0;

        std::string m_path;
        std::string m_name;

        std::vector<Abr::IndexRecord> m_indexRecords;

        char *m_dataBuf = nullptr; // buffer for data chunk, it have to be m_maxWriteSize in size
        uint32_t m_dataBufLength = 0;

        Handle m_hIndexFile = nullptr; // handle of the index file
    
        Handle m_hDataFile = nullptr; // handle of the data file
        uint64_t m_dataFileLength = 0; // current length of the data file

        // Set once create() has put the file into the repository, cleared when the
        // backup is committed or discarded. Tells discardPartialBackup() which
        // files it has to remove after their handles have been closed.
        bool m_indexFileCreated = false;
        bool m_dataFileCreated = false;

        uint8_t m_encryptedKey[32]; // data encryption key wrapped with user provided encryption key

        uint8_t m_cryptoKey[32]; // data encryption key for AES-256-GCM
        uint8_t m_cryptoIV[16];
    };

    class RestoreCore : public Abr::IRestore
    {
    public:
        RestoreCore(std::shared_ptr<IBackupManager> pManager)
            : m_pManager(pManager)
        {}

        virtual ~RestoreCore();

        virtual bool open(const std::string &path, const std::string &name, const std::string decryptionKey = "");

        const BackupInfo& getInfo() override {
            return m_info;
        }

        bool getDataRanges(std::vector<Range> &ranges) override;

        bool loadDataRange(uint32_t rangeIndex, void *pDataBuf, uint32_t length, uint32_t offset) override;

        bool checkDataRange(uint32_t rangeIndex, std::vector<char> &tempBuf) override;

        bool close() override;
        
        bool remove() override;

    protected:
        virtual Handle openFile(const std::string &fullName) = 0;

        virtual bool readFile(Handle hFile, void *pDataBuf, uint64_t offset, uint32_t dataLength) = 0;

        virtual bool closeFile(Handle hFile) = 0;

        virtual bool deleteFile(const std::string &fullName) = 0;

        int decryptData(const void *input, int inlen, void *output, int outlen, uint64_t offset);

    protected:
        std::shared_ptr<IBackupManager> m_pManager;
        BackupInfo m_info = {};

        std::string m_path;
        std::string m_name;

        Abr::IndexRecord *m_pIndexRecords = nullptr;
        uint32_t m_numIndexRecords = 0;

        Handle m_hDataFile = nullptr; // handle of the data file

        uint8_t m_cryptoKey[32];
        uint8_t m_cryptoIV[16];
    };    
}
