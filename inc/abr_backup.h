// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor backup definitions
//
#pragma once

#include <stdint.h>
#include <vector>
#include <string>
#include <memory>

namespace Abr
{
    struct BackupInfo {
        uint64_t sectors; // total number of sectors in the original data

        uint16_t sectorSize; // sector size of the original data

        uint16_t phySectorSize; // physical sector size of the original data

        uint32_t alignment; // alignment of data chunks in the backup file

        uint32_t minDataBlock; // minimum size of a data block in the backup file

        uint32_t maxDataBlock; // maximum size of a data block in the backup file

        uint8_t compression; // 0 - none, 1-9 - Zstd compression level for the backup data

        uint8_t encryption; // 0 - none, 1 - AES-256-GCM encryption  

        // 0 - full, 1 - incremental. An incremental holds only the blocks that
        // changed since its parent, so it is not restorable on its own.
        uint8_t incremental;

        uint64_t orig_data_size; // total size of the original data in bytes

        uint64_t stored_data_size; // total size of the stored and optionally compressed data in bytes
    };

    //
    // Backup interface
    //
    class IBackup
    {
    public:
        virtual const BackupInfo& getInfo() = 0;

        virtual bool saveDataRange(void *pDataBuf, uint32_t dataLength, uint64_t origOffset) = 0;

        virtual bool finalizeBackup(bool abort = false) = 0;
    };

    //
    // Restore interface
    //
    struct Range {
        Range(uint64_t offset_, uint32_t length_)
            : offset(offset_)
            , length(length_)
        {}

        uint64_t offset;
        uint32_t length;
    };

    class IRestore
    {
    public:
        virtual const BackupInfo& getInfo() = 0;

        virtual bool getDataRanges(std::vector<Range> &ranges) = 0;

        virtual bool loadDataRange(uint32_t rangeIndex, void *pDataBuf, uint32_t length, uint32_t offset) = 0;

        virtual bool checkDataRange(uint32_t rangeIndex, std::vector<char> &tempBuf) = 0;

        virtual bool close() = 0;

        virtual bool remove() = 0;
    };


    struct BackupManagerParams
    {
        uint32_t m_size; // size of the structure, used for versioning and validation

        bool m_createFolderIfMissing;

        std::string m_folder; // folder path or bucket name
    };

    //
    // Interface of a backup manager
    //
    class IBackupManager
    {
    public:
        virtual ~IBackupManager() = default;

        virtual bool initialize(const BackupManagerParams& params) = 0;
        
        virtual bool listBackups(const std::string &path, std::vector<std::string> &backups) = 0;

        virtual bool createBackup(const std::string &path, const std::string &name, const BackupInfo& info,
                         std::shared_ptr<IBackup> &pBackup, const std::string encryptionKey = "") = 0;
        
        virtual bool openBackup(const std::string &path, const std::string &name,
                         std::shared_ptr<IRestore> &pRestore, const std::string decryptionKey = "") = 0;

        virtual bool loadMetaData(const std::string &path, const std::string &name, std::ostream &metaData) = 0;

        virtual bool saveMetaData(const std::string &path, const std::string &name, std::istream &metaData) = 0;

        virtual bool deleteBackup(const std::string &path, const std::string &name) = 0;

        const std::string& getFolder() const {
            return m_folder;
        }

    protected:
        std::string m_folder;
    };

    //
    // Factory method to create a backup manager instance based on the specified mode and parameters
    //
    std::shared_ptr<Abr::IBackupManager> createBackupManager(const std::string &mode, const Abr::BackupManagerParams& params);
}
