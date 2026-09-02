// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Backup and restore core classes
//

#include "backup_core.h"
#include <iostream>
#include <memory>
#include <algorithm>
#include <cstring>
#include <assert.h>
#include <zstd.h>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include "crc32c/crc32c.h"

static_assert(sizeof(Abr::IndexHeader) == Abr::INDEX_FILE_HEADER_SIZE, "ABR IndexHeader size mismatch");

static bool deriveEncryptionKey(const std::string &password, uint8_t derivedKey[32])
{
    unsigned int digestLen = 0;

    if (!EVP_Digest(reinterpret_cast<const unsigned char*>(password.data()), password.size(),
                    derivedKey, &digestLen, EVP_sha256(), nullptr)) {
        return false;
    }

    return digestLen == 32;
}

static bool wrapEncryptionKey(const uint8_t *key, size_t keyLen, const std::string &password,
                              uint8_t *wrappedKey, uint8_t *iv)
{
    uint8_t derivedKey[32] = {};
    if (!deriveEncryptionKey(password, derivedKey)) {
        return false;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }

    int rv = 0;
    int len = 0;
    int finalLen = 0;

    rv = EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, nullptr, nullptr);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_CIPHER_CTX_set_padding(ctx, 0);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_EncryptInit_ex(ctx, nullptr, nullptr, derivedKey, iv);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_EncryptUpdate(ctx, wrappedKey, &len, key, static_cast<int>(keyLen));
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_EncryptFinal_ex(ctx, wrappedKey + len, &finalLen);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    EVP_CIPHER_CTX_free(ctx);
    return true;
}

static bool unwrapEncryptionKey(const uint8_t *wrappedKey, size_t keyLen, const std::string &password,
                                uint8_t *key, const uint8_t *iv)
{
    uint8_t derivedKey[32] = {};
    if (!deriveEncryptionKey(password, derivedKey)) {
        return false;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }

    int rv = 0;
    int len = 0;
    int finalLen = 0;

    rv = EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, nullptr, nullptr);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_CIPHER_CTX_set_padding(ctx, 0);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_DecryptInit_ex(ctx, nullptr, nullptr, derivedKey, iv);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_DecryptUpdate(ctx, key, &len, wrappedKey, static_cast<int>(keyLen));
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    rv = EVP_DecryptFinal_ex(ctx, key + len, &finalLen);
    if (rv != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    EVP_CIPHER_CTX_free(ctx);
    return true;
}

std::string join_backup_path(const std::string &path, const std::string &name)
{
    if (path.empty()) {
        return name;
    }
    if (name.empty()) {
        return path;
    }
    return path + "/" + name;
}

Abr::BackupCore::~BackupCore()
{
    if (m_dataBuf) {
        delete[] m_dataBuf;
    }

    // Nothing may be left over here: the file operations needed to remove it are
    // pure virtual, so the derived destructor is the last place that can clean up
    // and it is required to have called discardPartialBackup().
    assert(m_hIndexFile == nullptr);
    assert(m_hDataFile == nullptr);
    assert(!m_indexFileCreated && !m_dataFileCreated);
}

bool Abr::BackupCore::discardPartialBackup()
{
    bool ok = true;

    // The index goes first: while it is absent the data file is not visible as a
    // backup at all, so a cleanup that is itself interrupted never leaves a
    // listed but unrestorable backup behind.
    if (m_hIndexFile) {
        ok = closeFile(m_hIndexFile, true) && ok;
        m_hIndexFile = nullptr;
    } else if (m_indexFileCreated) {
        ok = deleteFile(join_backup_path(m_path, m_name) + Abr::INDEX_FILE_EXT) && ok;
    }
    m_indexFileCreated = false;

    if (m_hDataFile) {
        ok = closeFile(m_hDataFile, true) && ok;
        m_hDataFile = nullptr;
    } else if (m_dataFileCreated) {
        ok = deleteFile(join_backup_path(m_path, m_name) + Abr::DATA_FILE_EXT) && ok;
    }
    m_dataFileCreated = false;

    m_dataBufLength = 0;
    m_dataFileLength = 0;
    m_indexRecords.clear();

    if (!ok) {
        std::cerr << "Failed to remove the partial backup '" << m_name << "', it has to be deleted manually!"
                  << std::endl;
    }

    return ok;
}

bool Abr::BackupCore::create(const std::string &path, const std::string &name,
     const BackupInfo &info, const std::string encryptionKey)
{
    if (info.sectors == 0 || info.sectorSize == 0) {
        std::cerr << "Invalid backup info: sectors and sectorSize must be greater than zero" << std::endl;
        return false;
    }
    if (info.sectorSize != 512 && info.sectorSize != 4096) {
        std::cerr << "Invalid backup info: sectorSize must be 512 or 4096" << std::endl;
        return false;
    }
    if (info.phySectorSize % info.sectorSize || !info.phySectorSize) {
        std::cerr << "Invalid backup info: phySectorSize must be a multiple of sectorSize" << std::endl;
        return false;
    }
    if (info.alignment > 0x100000) {
        std::cerr << "Invalid backup info: alignment is too large" << std::endl;
        return false;
    }

    // finalizeBackup() records the data file name in the fixed size name list of
    // the index header. A name that does not fit there could not be written back,
    // so it is refused here rather than after a whole backup has been written.
    if (name.size() + strlen(Abr::DATA_FILE_EXT) >= sizeof(Abr::IndexHeader::data_file_names)) {
        std::cerr << "Invalid backup name: it needs " << (name.size() + strlen(Abr::DATA_FILE_EXT))
                  << " bytes with the data file extension, the name list of the index header holds "
                  << sizeof(Abr::IndexHeader::data_file_names) - 1 << std::endl;
        return false;
    }

    if (info.compression != 0) {
        if (info.compression > 22) {
            std::cerr << "Unsupported Zstd compression level " << info.compression << std::endl;
            return false;
        }
    }

    if (info.encryption) {
        if (info.encryption > 1) {
            std::cerr << "Unsupported encryption algorithm " << info.encryption << std::endl;
            return false;
        }
        if (encryptionKey.empty()) {
            std::cerr << "Encryption key is required for algorithm " << info.encryption << std::endl;
            return false;
        }

        // Generate random data encryption key and IV for AES-256-GCM
        RAND_bytes(m_cryptoKey, sizeof(m_cryptoKey));
        RAND_bytes(m_cryptoIV, sizeof(m_cryptoIV));

        if (!wrapEncryptionKey(m_cryptoKey, sizeof(m_cryptoKey), encryptionKey, m_encryptedKey, m_cryptoIV)) {
            std::cerr << "Failed to wrap encryption key" << std::endl;
            return false;
        }
    }

    m_path = path;
    m_name = name;
    m_info = info;

    m_info.orig_data_size = 0;
    m_info.stored_data_size = 0;
    
    if (m_maxWriteSize) {
        m_dataBuf = new char[m_maxWriteSize];
        if (!m_dataBuf) {
            std::cerr << "Failed to allocate data buffer of size " << m_maxWriteSize << std::endl;
            return false;
        }
        m_dataBufLength = 0;
    }

    // Create index file
    std::string fullPath = join_backup_path(path, name) + Abr::INDEX_FILE_EXT;

    m_hIndexFile = createFile(fullPath, false);
    if (!m_hIndexFile) {
        std::cerr << "Failed to create index file " << fullPath << std::endl;
        return false;
    }
    m_indexFileCreated = true;

    // Create data file
    fullPath = join_backup_path(path, name) + Abr::DATA_FILE_EXT;

    m_hDataFile = createFile(fullPath, true);
    if (!m_hDataFile) {
        std::cerr << "Failed to create backup file " << fullPath << std::endl;
        // The caller never gets a backup object it could abort, so the index file
        // created above would stay behind as an orphan.
        discardPartialBackup();
        return false;
    }
    m_dataFileCreated = true;

    return true;
}

int Abr::BackupCore::encryptData(const void *input, int inlen, void *output, int outlen, uint64_t offset)
{
    int finalLen = 0;

    if (outlen < inlen || outlen - inlen < 16) {
        std::cerr << "Output buffer is too small for encryption" << std::endl;
        return 0;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        std::cerr << "Failed to create AES-GCM context" << std::endl;
        return 0;
    }

    int rv = 0;
    int len = 0;

    do {
        unsigned char iv[16];
        memcpy(iv, m_cryptoIV, sizeof(iv));
        for (int i = 0; i < sizeof(offset); ++i) {
            iv[i] ^= (offset >> (i * 8)) & 0xFF;
        }
        
        rv = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        if (rv != 1) {
            std::cerr << "Failed to init AES-GCM cipher" << std::endl;
            break;
        }

        rv = EVP_EncryptInit_ex(ctx, nullptr, nullptr, m_cryptoKey, iv);
        if (rv != 1) {
            std::cerr << "Failed to init AES-GCM key/IV" << std::endl;
            break;
        }

        rv = EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(output), &len,
                                reinterpret_cast<const unsigned char*>(input), inlen);
        if (rv != 1) {
            std::cerr << "Failed to encrypt data with AES-GCM" << std::endl;
            break;
        }

        rv = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(output) + len, &finalLen);
        if (rv != 1) {
            std::cerr << "Failed to finalize AES-GCM encryption" << std::endl;
            break;
        }

        finalLen += len;

        rv = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, reinterpret_cast<unsigned char*>(output) + finalLen);
        if (rv != 1) {
            std::cerr << "Failed to get AES-GCM tag" << std::endl;
            break;
        }

        finalLen += 16;
    } while (0);

    EVP_CIPHER_CTX_free(ctx);

    return (rv == 1) ? finalLen : 0;
}

bool Abr::BackupCore::saveDataRange(void *pDataBuf, uint32_t dataLength, uint64_t origOffset)
{
    uint32_t blockSize = std::max(m_info.phySectorSize, (uint16_t)0x1000);
    blockSize = std::max(blockSize, m_info.minDataBlock);

    // Dropping all-zero blocks is only safe when a missing block means "zero",
    // which is what a full backup of a whole disk says. In an incremental a
    // missing block means "unchanged", so a block that changed from data to
    // zeroes has to be stored: skipping it would leave the parent's stale data
    // in place at restore time.
    const bool skipEmptyBlocks = !m_info.incremental;

    while (dataLength) {
        // remove empty blocks from the data
        uint32_t length = skipEmptyBlocks ? findEmptyBlockLength((uint8_t *)pDataBuf, dataLength, blockSize) : 0;
        if (length) {
            origOffset += length;
            dataLength -= length;
            if (dataLength == 0) {
                // the block is empty, we can skip it without writing to the backup file and creating index record
                return true;
            }
            pDataBuf = (uint8_t *)pDataBuf + length;
        }

        length = skipEmptyBlocks ? findNonEmptyBlockLength((uint8_t *)pDataBuf, dataLength, blockSize) : dataLength;

        if (length == 0) {
            // What is left is shorter than a block and holds no data: the empty
            // block scan reports whole blocks only, so it left this tail behind.
            // A block that is not in the backup reads back as zeroes, so the tail
            // is dropped like any other empty block.
            //
            // This is also what guarantees the loop terminates. Both scans
            // returning zero would consume nothing, and the loop would spin while
            // appending an index record on every pass.
            return true;
        }

        if (m_info.maxDataBlock && length > m_info.maxDataBlock) {
            length = m_info.maxDataBlock;
        }

        // Prepare index record for this chunk (orig length is 'length')
        Abr::IndexRecord record(origOffset, length);
        record.chunk_offset = m_dataFileLength + m_dataBufLength;

        // By default assume no compression
        uint32_t storedLen = length;
        const uint8_t *writePtr = (const uint8_t*)pDataBuf;
        std::unique_ptr<uint8_t[]> compressedBuf;

        m_info.orig_data_size += storedLen;

        if (m_info.compression) {
            size_t maxCSize = ZSTD_compressBound(length);

            compressedBuf.reset(new (std::nothrow) uint8_t[maxCSize]);
            if (!compressedBuf) {
                std::cerr << "Memory alloc error for compression!" << std::endl;
                return false;
            }

            size_t cSize = ZSTD_compress(compressedBuf.get(), maxCSize, pDataBuf, length, m_info.compression);
            if (ZSTD_isError(cSize)) {
                std::cerr << "ZSTD compression error: " << ZSTD_getErrorName(cSize) << std::endl;
                return false;
            }

            if (cSize > UINT32_MAX) {
                std::cerr << "Compressed chunk too large" << std::endl;
                return false;
            }

            storedLen = (uint32_t)cSize;
            writePtr = compressedBuf.get();
        }

        std::unique_ptr<uint8_t[]> encryptedBuf;

        if (m_info.encryption) {
            uint32_t bufLen = storedLen + 16;
            
            encryptedBuf.reset(new (std::nothrow) uint8_t[bufLen]);
            if (!encryptedBuf) {
                std::cerr << "Memory alloc error for encryption!" << std::endl;
                return false;
            }

            storedLen = encryptData(writePtr, storedLen, encryptedBuf.get(), bufLen, origOffset);
            if (storedLen == 0) {
                std::cerr << "Failed to encrypt backup chunk at offset " << origOffset << std::endl;
                return false;
            }

            writePtr = encryptedBuf.get();
        }

        record.chunk_length = storedLen;

        // Counted after both transformations, so it describes the size of the
        // data file: the compressed size of the chunk plus the authentication
        // tag the encryption appends to it.
        m_info.stored_data_size += storedLen;

        // Calculate CRC32C for stored chunk (compressed/raw/encrypted)
        record.chunk_crc32c = crc32c::Crc32c(writePtr, storedLen);

        m_indexRecords.emplace_back(record);
        origOffset += length; // advance by original (uncompressed) length

        // Write stored data (either compressed buffer or original data)
        uint32_t remaining = storedLen;
        const uint8_t *bufPtr = writePtr;

        if (m_minWriteSize) {
            // buffered write
            while (remaining) {
                uint32_t lenToCopy = remaining;
                if (m_dataBufLength + lenToCopy > m_maxWriteSize) {
                    lenToCopy = m_maxWriteSize - m_dataBufLength;
                }

                memcpy(m_dataBuf + m_dataBufLength, bufPtr, lenToCopy);
                m_dataBufLength += lenToCopy;

                remaining -= lenToCopy;
                bufPtr += lenToCopy;

                if (m_minWriteSize > m_dataBufLength) {
                    continue;
                }

                if (!writeFile(m_hDataFile, m_dataBuf, m_dataBufLength)) {
                    return false;
                }

                m_dataFileLength += m_dataBufLength;
                m_dataBufLength = 0;
            }
        } else {
            // direct write
            if (!writeFile(m_hDataFile, (void*)bufPtr, remaining)) {
                return false;
            }
            m_dataFileLength += remaining;
            remaining = 0;
        }

        // consume original input data
        dataLength -= length;
        pDataBuf = (uint8_t *)pDataBuf + length;
    }

    return true;
}

bool Abr::BackupCore::finalizeBackup(bool abort)
{
    if (abort) {
        return discardPartialBackup();
    }

    if (!m_hIndexFile) {
        // Already finalized or already discarded, there is nothing to commit and
        // no data of this backup left in the repository.
        std::cerr << "Backup '" << m_name << "' is not open, it cannot be finalized!" << std::endl;
        return false;
    }

    if (m_minWriteSize && m_dataBufLength) {
        // write the last part of the data
        if (!writeFile(m_hDataFile, m_dataBuf, m_dataBufLength)) {
            std::cerr << "Failed to write the last data chunk of backup '" << m_name << "'!\n";
            discardPartialBackup();
            return false;
        }

        m_dataFileLength += m_dataBufLength;
        m_dataBufLength = 0;
    }

    // The data file has to be closed before the index that points into it is
    // written: on S3 closing is the multipart completion, and a failed completion
    // leaves no data file at all, so the index would describe a backup that is
    // not there.
    bool dataFileClosed = closeFile(m_hDataFile);
    m_hDataFile = nullptr;

    if (!dataFileClosed) {
        std::cerr << "Failed to close the data file of backup '" << m_name << "'!\n";
        discardPartialBackup();
        return false;
    }

    uint32_t indexCount = m_indexRecords.size();
    uint64_t indexSize = Abr::INDEX_FILE_RECORD_SIZE * indexCount;

    char *pBuf = new (std::nothrow) char[Abr::INDEX_FILE_HEADER_SIZE + indexSize];
    if (!pBuf) {
        std::cerr << "Memory alloc error!\n";
        discardPartialBackup();
        return false;
    }

    auto pHeader = reinterpret_cast<Abr::IndexHeader *>(pBuf);

    memset(pHeader, 0, Abr::INDEX_FILE_HEADER_SIZE);
    memcpy(pHeader->signature, Abr::INDEX_FILE_SIGNATURE, sizeof(pHeader->signature));
    pHeader->version = Abr::INDEX_FILE_VERSION;
    pHeader->compression = m_info.compression;
    pHeader->encryption = m_info.encryption;
    pHeader->incremental = m_info.incremental ? 1 : 0;
    pHeader->index_offset = sizeof(*pHeader);
    pHeader->index_count = indexCount;
    pHeader->image_size = m_info.sectors * m_info.sectorSize;
    pHeader->sector_size = m_info.sectorSize;
    pHeader->phy_sector_size = m_info.phySectorSize;
    pHeader->min_data_block = m_info.minDataBlock;
    pHeader->max_data_block = m_info.maxDataBlock;
    pHeader->orig_data_size = m_info.orig_data_size;
    pHeader->stored_data_size = m_info.stored_data_size;

    if (m_info.encryption) {
        memcpy(pHeader->encryption_key, m_encryptedKey, sizeof(m_encryptedKey));
        memcpy(pHeader->encryption_iv, m_cryptoIV, sizeof(m_cryptoIV));
    }
 
    pHeader->data_file_names_offset = (uint16_t)(pHeader->data_file_names - (uint8_t*)pHeader);

    // create() refuses a name that does not fit, so this cannot trigger; the copy
    // is bounded anyway, because overrunning the name list here would write over
    // the index records that follow the header. The header was zeroed above, so
    // the copy leaves the list terminated.
    const std::string dataFile = m_name + Abr::DATA_FILE_EXT;

    if (dataFile.size() >= sizeof(pHeader->data_file_names)) {
        std::cerr << "Backup name does not fit in the index header!\n";
        delete[] pBuf;
        discardPartialBackup();
        return false;
    }

    memcpy(pHeader->data_file_names, dataFile.c_str(), dataFile.size());

    auto pRecord = reinterpret_cast<Abr::IndexRecord *>(pBuf + pHeader->index_offset);

    // copy range records to the buffer
    for (auto &record : m_indexRecords) {
        *pRecord++ = record;
    }
    
    // create crc32c for the index records
    if (indexSize > 0) {
        pHeader->index_crc32c = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(pBuf + pHeader->index_offset), indexSize);
    }

    // create crc32c for the index header
    pHeader->header_crc32c = 0;
    pHeader->header_crc32c = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(pBuf), Abr::INDEX_FILE_HEADER_SIZE);

    if (!writeFile(m_hIndexFile, pBuf, Abr::INDEX_FILE_HEADER_SIZE + indexSize)) {
        std::cerr << "Failed to write backup file index '" << m_name << "'!\n";
        delete[] pBuf;
        discardPartialBackup();
        return false;
    }

    delete[] pBuf;

    if (!closeFile(m_hIndexFile)) {
        std::cerr << "Failed to close backup file index '" << m_name << "'!\n";
        m_hIndexFile = nullptr;
        discardPartialBackup();
        return false;
    }

    m_hIndexFile = nullptr;

    // From here on the backup is complete: it is a repository object of its own
    // and must not be touched by any later cleanup.
    m_indexFileCreated = false;
    m_dataFileCreated = false;

    //std::cout << "Backup file index '" << m_name << "' created\n";

    return true;
}

// Scan up to `count` bytes for any non-zero byte. Reads 8 bytes at a time but
// never past `count`, so it is safe even when count is not 8-aligned or smaller
// than a block.
inline bool blockHasData(const uint8_t *buf, uint32_t count)
{
    for (; count && *buf == 0; count--, buf++) {}

    return count != 0;
}

uint32_t Abr::BackupCore::findEmptyBlockLength(uint8_t *buf, uint32_t length, uint32_t blockSize)
{
    // Return the length of the leading run of whole empty blocks. The final
    // partial block (length not a multiple of blockSize) is never reported as a
    // skippable empty block, matching the original blockSize-floored behaviour.
    uint32_t offset = 0;
    while (offset + blockSize <= length) {
        if (blockHasData(buf + offset, blockSize)) {
            break;
        }
        offset += blockSize;
    }
    return offset;
}

uint32_t Abr::BackupCore::findNonEmptyBlockLength(uint8_t *buf, uint32_t length, uint32_t blockSize)
{
    // Return the length of the leading run of non-empty blocks. The scan is
    // clamped to `length` for both the per-block read and the running offset, so
    // it never reads past the buffer nor returns more than the caller supplied.
    uint32_t offset = 0;
    while (offset < length) {
        uint32_t blockLen = std::min(blockSize, length - offset);
        if (!blockHasData(buf + offset, blockLen)) {
            break;
        }
        offset += blockLen;
    }
    return offset;
}

///////

Abr::RestoreCore::~RestoreCore()
{
    if (m_pIndexRecords) {
        delete[] m_pIndexRecords;
    }

    assert(m_hDataFile == nullptr);
}

bool Abr::RestoreCore::open(const std::string &path, const std::string &name, const std::string decryptionKey)
{
    std::string indexPath = join_backup_path(path, name) + Abr::INDEX_FILE_EXT;

    Handle hIndexFile = openFile(indexPath);
    if (!hIndexFile) {
        std::cerr << "Failed to open index file " << indexPath << std::endl;
        return false;
    }

    Abr::IndexHeader indexHeader;

    if (!readFile(hIndexFile, &indexHeader, 0, sizeof(indexHeader))) {
        std::cerr << "Failed to read index header from file " << indexPath << std::endl;
        closeFile(hIndexFile);
        return false;
    }

    if (memcmp(indexHeader.signature, Abr::INDEX_FILE_SIGNATURE, sizeof(indexHeader.signature)) != 0) {
        std::cerr << "Invalid index file signature in file " << indexPath << std::endl;
        closeFile(hIndexFile);
        return false;
    }

    if (indexHeader.version != Abr::INDEX_FILE_VERSION) {
        std::cerr << "Invalid index file version in file " << indexPath << std::endl;
        closeFile(hIndexFile);
        return false;
    }

    if (indexHeader.header_crc32c) {
        uint32_t savedHeaderCrc = indexHeader.header_crc32c;
        uint32_t computedHeaderCrc = 0;

        indexHeader.header_crc32c = 0;
        computedHeaderCrc = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(&indexHeader), sizeof(indexHeader));
        if (computedHeaderCrc != savedHeaderCrc) {
            std::cerr << "Header CRC32C mismatch in file " << indexPath << std::endl;
            closeFile(hIndexFile);
            return false;
        }
        indexHeader.header_crc32c = savedHeaderCrc;
    }

    m_info.sectorSize = indexHeader.sector_size;
    m_info.phySectorSize = indexHeader.phy_sector_size;
    m_info.maxDataBlock = indexHeader.max_data_block;
    m_info.minDataBlock = indexHeader.min_data_block;
    m_info.compression = indexHeader.compression;
    m_info.encryption = indexHeader.encryption;
    m_info.incremental = indexHeader.incremental;
    m_info.orig_data_size = indexHeader.orig_data_size;
    m_info.stored_data_size = indexHeader.stored_data_size;
    
    if (m_info.sectorSize != 512 && m_info.sectorSize != 4096) {
        std::cerr << "Invalid sector size in file " << indexPath << std::endl;
        closeFile(hIndexFile);
        return false;
    }

    if (m_info.compression) {
        // Support only zstd for now
        if (m_info.compression > 22) {
            std::cerr << "Unsupported zstd compression level " << m_info.compression << std::endl;
            closeFile(hIndexFile);
            return false;
        }
    }

    if (m_info.encryption) {
        if (m_info.encryption > 1) {
            std::cerr << "Unsupported encryption algorithm " << m_info.encryption << std::endl;
            closeFile(hIndexFile);
            return false;
        }

        memcpy(m_cryptoIV, indexHeader.encryption_iv, sizeof(m_cryptoIV));
        memset(m_cryptoKey, 0, sizeof(m_cryptoKey));

        if (!decryptionKey.empty()) {
            // Unwrap the encryption key using the provided decryption key
            if (!unwrapEncryptionKey(indexHeader.encryption_key, sizeof(indexHeader.encryption_key),
                                    decryptionKey, m_cryptoKey, m_cryptoIV)) {
                std::cerr << "Failed to unwrap encryption key" << std::endl;
                closeFile(hIndexFile);
                return false;
            }
        }
    }

    m_info.sectors = indexHeader.image_size / m_info.sectorSize;

    m_path = path;
    m_name = name;
   
    m_numIndexRecords = indexHeader.index_count;
    uint32_t indexSize = m_numIndexRecords * Abr::INDEX_FILE_RECORD_SIZE;

    // Read indexes. A backup that stored nothing - an image without any data, or
    // a no-change incremental - has none of them, and is a valid backup with an
    // empty data file.
    if (m_numIndexRecords) {
        m_pIndexRecords = new (std::nothrow) Abr::IndexRecord[m_numIndexRecords];
        if (!m_pIndexRecords) {
            std::cerr << "Memory alloc error!\n";
            closeFile(hIndexFile);
            return false;
        }

        if (!readFile(hIndexFile, m_pIndexRecords, indexHeader.index_offset, indexSize)) {
            std::cerr << "Failed to read index records from file " << indexPath << std::endl;
            delete[] m_pIndexRecords;
            m_pIndexRecords = nullptr;
            closeFile(hIndexFile);
            return false;
        }
    }

    closeFile(hIndexFile);

    if (indexSize > 0 && indexHeader.index_crc32c) {
        uint32_t computedIndexCrc = 0;
        computedIndexCrc = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(m_pIndexRecords), indexSize);
        if (computedIndexCrc != indexHeader.index_crc32c) {
            std::cerr << "Index records CRC32C mismatch in file " << indexPath << std::endl;
            delete[] m_pIndexRecords;
            m_pIndexRecords = nullptr;
            return false;
        }
    }

    std::string dataPath = join_backup_path(path, name) + Abr::DATA_FILE_EXT;

    m_hDataFile = openFile(dataPath);
    if (!m_hDataFile) {
        std::cerr << "Failed to open data file " << dataPath << std::endl;
        return false;
    }

    return true;
}

bool Abr::RestoreCore::getDataRanges(std::vector<Range> &ranges)
{
    if (ranges.size()) {
        std::cerr << "Vector is not empty!\n";
        return false;
    }

    Abr::IndexRecord *pRecord = m_pIndexRecords;

    for (int i = 0; i < m_numIndexRecords; i++) {
        ranges.emplace_back(Range(pRecord[i].orig_offset, pRecord[i].orig_length));
    }

    return true;
}

int Abr::RestoreCore::decryptData(const void *input, int inlen, void *output, int outlen, uint64_t offset)
{
    int finalLen = 0;

    if ( inlen <= 16 || outlen < (inlen - 16)) {
        std::cerr << "Input buffer length is invalid!" << std::endl;
        return 0;
    }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        std::cerr << "Failed to create AES-GCM context" << std::endl;
        return 0;
    }

    int rv = 0;
    int len = 0;

    inlen -= 16; // last 16 bytes are the tag
    unsigned char *tag = reinterpret_cast<unsigned char *>(const_cast<void *>(input)) + inlen;

    do {
        unsigned char iv[16];
        memcpy(iv, m_cryptoIV, sizeof(iv));
        for (int i = 0; i < sizeof(offset); ++i) {
            iv[i] ^= (offset >> (i * 8)) & 0xFF;
        }

        rv = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        if (rv != 1) {
            std::cerr << "Failed to init AES-GCM cipher" << std::endl;
            break;
        }

        rv = EVP_DecryptInit_ex(ctx, nullptr, nullptr, m_cryptoKey, iv);
        if (rv != 1) {
            std::cerr << "Failed to init AES-GCM key/IV" << std::endl;
            break;
        }

        rv = EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(output), &len,
                                reinterpret_cast<const unsigned char*>(input), inlen);
        if (rv != 1) {
            std::cerr << "Failed to decrypt data with AES-GCM" << std::endl;
            break;
        }

        rv = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
        if (rv != 1) {
            std::cerr << "Failed to set AES-GCM tag" << std::endl;
            break;
        }

        rv = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(output) + len, &finalLen);
        if (rv != 1) {
            std::cerr << "Failed to finalize AES-GCM decryption (" << ERR_peek_last_error() << ")" << std::endl;
            break;
        }

        finalLen += len;
    } while (0);

    EVP_CIPHER_CTX_free(ctx);

    return (rv == 1) ? finalLen : 0;
}

bool Abr::RestoreCore::loadDataRange(uint32_t rangeIndex, void *pDataBuf, uint32_t length, uint32_t offset)
{
    if (rangeIndex >= m_numIndexRecords) {
        std::cerr << "Invalid range index " << rangeIndex << "!\n";
        return false;
    }
    auto &record = m_pIndexRecords[rangeIndex];

    if (offset > record.orig_length || length > record.orig_length - offset) {
        std::cerr << "Invalid data len " << length << " and offset " << offset << " for index " << rangeIndex << "!\n";
        return false;
    }

    if (m_info.compression == 0 && m_info.encryption == 0 && record.chunk_crc32c != 0 && 
        offset == 0 && length == record.orig_length && record.chunk_length == record.orig_length) {
        // Fast path: uncompressed full-chunk read with CRC
        
        if (!readFile(m_hDataFile, pDataBuf, record.chunk_offset, length)) {
            std::cerr << "Failed to read from data file!" << std::endl;
            return false;
        }

        uint32_t computedCrc = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(pDataBuf), length);
        if (computedCrc != record.chunk_crc32c) {
            std::cerr << "CRC32C mismatch for restored chunk " << rangeIndex << "!" << std::endl;
            return false;
        }

        return true;
    }

    // Read stored chunk (compressed or raw) into temporary buffer
    std::unique_ptr<char[]> storedBuf(new (std::nothrow) char[record.chunk_length]);
    if (!storedBuf) {
        std::cerr << "Memory alloc error!" << std::endl;
        return false;
    }

    if (!readFile(m_hDataFile, storedBuf.get(), record.chunk_offset, record.chunk_length)) {
        std::cerr << "Failed to read from data file!" << std::endl;
        return false;
    }

    if (record.chunk_crc32c != 0) {
        uint32_t computedIndexCrc = 0;
        computedIndexCrc = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(storedBuf.get()), record.chunk_length);
        if (computedIndexCrc != record.chunk_crc32c) {
            std::cerr << "CRC32C mismatch for restored chunk " << rangeIndex << "!" << std::endl;
            return false;
        }
    }

    std::unique_ptr<char[]> payloadBuf;
    const char *payloadPtr = storedBuf.get();
    int bufLen = record.chunk_length;

    if (m_info.encryption) {
        payloadBuf.reset(new (std::nothrow) char[bufLen]);
        if (!payloadBuf) {
            std::cerr << "Memory alloc error!" << std::endl;
            return false;
        }

        bufLen = decryptData(storedBuf.get(), record.chunk_length, payloadBuf.get(), bufLen, record.orig_offset);
        if (bufLen <= 0) {
            std::cerr << "Failed to decrypt backup chunk " << rangeIndex << "!" << std::endl;
            return false;
        }

        payloadPtr = payloadBuf.get();
    }

    if (m_info.compression == 0) {
        memcpy(pDataBuf, payloadPtr + offset, length);
        return true;
    }

    // zstd decompression
    std::unique_ptr<char[]> decompBuf(new (std::nothrow) char[record.orig_length]);
    if (!decompBuf) {
        std::cerr << "Memory alloc error!" << std::endl;
        return false;
    }

    size_t dSize = ZSTD_decompress(decompBuf.get(), record.orig_length, payloadPtr, bufLen);
    if (ZSTD_isError(dSize)) {
        std::cerr << "ZSTD decompression error: " << ZSTD_getErrorName(dSize) << std::endl;
        return false;
    }
    if (dSize != record.orig_length) {
        std::cerr << "Decompressed size mismatch for restored chunk " << rangeIndex << "!" << std::endl;
        return false;
    }

    memcpy(pDataBuf, decompBuf.get() + offset, length);
    return true;
}

bool Abr::RestoreCore::checkDataRange(uint32_t rangeIndex, std::vector<char> &tempBuf)
{
    if (rangeIndex >= m_numIndexRecords) {
        std::cerr << "Invalid range index " << rangeIndex << "!\n";
        return false;
    }

    auto &record = *(m_pIndexRecords + rangeIndex);

    if (record.chunk_crc32c == 0) {
        // It's an old backup without CRC, so we cannot verify the data integrity
        return true;
    }

    // Read stored chunk into temporary buffer

    if (tempBuf.size() < record.chunk_length) {
        tempBuf.resize(record.chunk_length);
    }

    if (!readFile(m_hDataFile, tempBuf.data(), record.chunk_offset, record.chunk_length)) {
        std::cerr << "Failed to read from data file!" << std::endl;
        return false;
    }

    uint32_t computedIndexCrc = 0;
    computedIndexCrc = crc32c::Crc32c(reinterpret_cast<const uint8_t *>(tempBuf.data()), record.chunk_length);
    if (computedIndexCrc != record.chunk_crc32c) {
        std::cerr << "CRC32C mismatch for restored chunk " << rangeIndex << "!" << std::endl;
        return false;
    }
    
    return true;
}

bool Abr::RestoreCore::remove()
{
    close();
    
    std::string fullPath = join_backup_path(m_path, m_name) + Abr::DATA_FILE_EXT;

    if (!deleteFile(fullPath)) {
        std::cerr << "Failed to delete data file " << fullPath << std::endl;
        return false;
    }

    fullPath = join_backup_path(m_path, m_name) + Abr::INDEX_FILE_EXT;

    if (!deleteFile(fullPath)) {
        std::cerr << "Failed to delete index file " << fullPath << std::endl;
        return false;
    }

    return true;
}

bool Abr::RestoreCore::close()
{
    if (m_hDataFile) {
        closeFile(m_hDataFile);
        m_hDataFile = nullptr;
    }
    return true;
}
