// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Tests for encrypted backups
//
// Encryption has to hold up against more than a round trip: the plaintext must
// not be recoverable from the repository, a wrong key must fail instead of
// returning rubbish, and altered ciphertext must be rejected even when every
// checksum over it has been repaired - that last one is what the GCM tag is for
// and what a plain checksum cannot do.
//

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "core_fixture.h"

namespace
{
    // 32 bytes, as an AES-256 key is derived from this by SHA-256 anyway.
    const std::string kKey = "0123456789abcdef0123456789abcdef";
    const std::string kOtherKey = "fedcba9876543210fedcba9876543210";

    class EncryptionTest : public ut::CoreTest
    {
    protected:
        static Abr::BackupInfo encryptedInfo(uint8_t compression = 0)
        {
            auto info = ut::defaultInfo();
            info.encryption = 1;
            info.compression = compression;
            return info;
        }
    };

    TEST_F(EncryptionTest, AnEncryptedBackupRoundTrips)
    {
        const auto first = ut::patternBuffer(0x1000, 0x1000, 'A');
        const auto second = ut::randomBuffer(0x1000, 0xabcd);

        writeBackup(encryptedInfo(), { { 0, first }, { 0x8000, second } }, kKey);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName, kKey));

        std::vector<Abr::Range> ranges;
        ASSERT_TRUE(restore->getDataRanges(ranges));
        ASSERT_EQ(ranges.size(), 2u);

        std::vector<char> loaded(0x1000, 0);

        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_EQ(loaded, first);

        ASSERT_TRUE(restore->loadDataRange(1, loaded.data(), loaded.size(), 0));
        EXPECT_EQ(loaded, second);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, TheHeaderRecordsTheAlgorithmAndKeyMaterial)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) } }, kKey);

        const auto header = readHeader();
        EXPECT_EQ(header.encryption, 1u);

        // The data key is random per backup and stored wrapped, so neither field
        // may be left at zero.
        const uint8_t zeroKey[sizeof(header.encryption_key)] = {};
        const uint8_t zeroIv[sizeof(header.encryption_iv)] = {};
        EXPECT_NE(memcmp(header.encryption_key, zeroKey, sizeof(zeroKey)), 0);
        EXPECT_NE(memcmp(header.encryption_iv, zeroIv, sizeof(zeroIv)), 0);

        // What is stored is the wrapped data key, never the key the caller gave.
        EXPECT_NE(memcmp(header.encryption_key, kKey.data(), sizeof(header.encryption_key)), 0);
    }

    TEST_F(EncryptionTest, TheStoredBytesAreNotThePlaintext)
    {
        const auto original = ut::patternBuffer(0x1000, 0x1000);
        writeBackup(encryptedInfo(), { { 0, original } }, kKey);

        const auto *stored = repo.find(dataFile());
        ASSERT_NE(stored, nullptr);
        ASSERT_GE(stored->size(), original.size());

        EXPECT_NE(memcmp(stored->data(), original.data(), original.size()), 0);
    }

    TEST_F(EncryptionTest, EachChunkCarriesAnAuthenticationTag)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) } }, kKey);

        const auto records = readRecords();
        ASSERT_EQ(records.size(), 1u);

        // AES-256-GCM appends a 16 byte tag to the ciphertext.
        EXPECT_EQ(records[0].chunk_length, records[0].orig_length + 16);
    }

    TEST_F(EncryptionTest, TheStoredChunkDecryptsWithTheDocumentedKeyAndNonce)
    {
        // Rebuilds the whole key and nonce chain with plain OpenSSL calls, so any
        // change in how the library derives them breaks this test before it can
        // break the backups already written with it:
        //   KEK   = SHA-256(secret)
        //   DEK   = AES-256-CTR(KEK, header IV) over the wrapped key
        //   nonce = first 12 bytes of the header IV, the chunk's original offset
        //           XORed little-endian into the first 8 of them
        //   chunk = AES-256-GCM(DEK, nonce) ciphertext followed by a 16 byte tag
        const uint64_t offset = 0x0102030405060000ull;
        const auto original = ut::randomBuffer(0x1000, 0x5eed);

        auto info = encryptedInfo();
        info.sectors = (offset + 0x10000) / info.sectorSize;
        writeBackup(info, { { offset, original } }, kKey);

        const auto header = readHeader();
        const auto records = readRecords();
        ASSERT_EQ(records.size(), 1u);
        ASSERT_EQ(records[0].orig_offset, offset);

        const auto *data = repo.find(ut::dataName(kPath, kName));
        ASSERT_NE(data, nullptr);

        unsigned char kek[32];
        unsigned int kekLen = 0;
        ASSERT_EQ(EVP_Digest(kKey.data(), kKey.size(), kek, &kekLen, EVP_sha256(), nullptr), 1);
        ASSERT_EQ(kekLen, 32u);

        unsigned char dek[32];
        int len = 0;
        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> holder(EVP_CIPHER_CTX_new(),
                                                                           EVP_CIPHER_CTX_free);
        EVP_CIPHER_CTX *ctx = holder.get();
        ASSERT_NE(ctx, nullptr);
        ASSERT_EQ(EVP_DecryptInit_ex(ctx, EVP_aes_256_ctr(), nullptr, kek, header.encryption_iv), 1);
        ASSERT_EQ(EVP_DecryptUpdate(ctx, dek, &len, header.encryption_key, sizeof(header.encryption_key)), 1);
        ASSERT_EQ(len, 32);

        unsigned char nonce[12];
        memcpy(nonce, header.encryption_iv, sizeof(nonce));
        for (int i = 0; i < 8; i++) {
            nonce[i] ^= static_cast<unsigned char>(offset >> (i * 8));
        }

        const auto *chunk = reinterpret_cast<const unsigned char *>(data->data() + records[0].chunk_offset);
        const int cipherLen = static_cast<int>(records[0].chunk_length) - 16;
        ASSERT_EQ(cipherLen, static_cast<int>(original.size()));

        std::vector<char> plain(original.size());
        ASSERT_EQ(EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr), 1);
        ASSERT_EQ(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(nonce), nullptr), 1);
        ASSERT_EQ(EVP_DecryptInit_ex(ctx, nullptr, nullptr, dek, nonce), 1);
        ASSERT_EQ(EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(plain.data()), &len, chunk, cipherLen), 1);
        ASSERT_EQ(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char *>(chunk + cipherLen)), 1);
        EXPECT_EQ(EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(plain.data()) + len, &len), 1)
            << "the authentication tag does not match the documented nonce";

        EXPECT_EQ(plain, original);
    }

    TEST_F(EncryptionTest, EveryBackupGetsFreshKeyMaterial)
    {
        const auto original = ut::patternBuffer(0x1000, 0x1000);

        writeBackup(encryptedInfo(), { { 0, original } }, kKey, "first");
        writeBackup(encryptedInfo(), { { 0, original } }, kKey, "second");

        const auto firstHeader = readHeader("first");
        const auto secondHeader = readHeader("second");

        // The same passphrase and the same plaintext must not produce the same
        // ciphertext: the data key and the IV are generated per backup.
        EXPECT_NE(memcmp(firstHeader.encryption_iv, secondHeader.encryption_iv,
                         sizeof(firstHeader.encryption_iv)), 0);
        EXPECT_NE(memcmp(firstHeader.encryption_key, secondHeader.encryption_key,
                         sizeof(firstHeader.encryption_key)), 0);
        EXPECT_NE(*repo.find(ut::dataName(kPath, "first")),
                  *repo.find(ut::dataName(kPath, "second")));
    }

    TEST_F(EncryptionTest, AWrongKeyDoesNotReturnData)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) } }, kKey);

        auto restore = newRestore();

        // Unwrapping is unauthenticated, so opening with the wrong key produces a
        // wrong data key rather than an error; the GCM tag is what stops the
        // wrong plaintext from being handed out.
        ASSERT_TRUE(restore->open(kPath, kName, kOtherKey));

        std::vector<char> loaded(0x1000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("Failed to decrypt backup chunk")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, AMissingKeyDoesNotReturnData)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) } }, kKey);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName));

        std::vector<char> loaded(0x1000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("Failed to decrypt backup chunk")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, TamperedCiphertextIsCaughtByTheChecksum)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) } }, kKey);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName, kKey));

        ASSERT_TRUE(repo.corrupt(dataFile(), 0x20));

        std::vector<char> loaded(0x1000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("CRC32C mismatch")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, TamperedCiphertextIsCaughtByTheAuthenticationTag)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) } }, kKey);

        // Alter the ciphertext and repair every checksum covering it, so the
        // backup looks intact to the integrity checks. Only the GCM tag can tell
        // that the data was changed by someone without the key.
        ASSERT_TRUE(repo.corrupt(dataFile(), 0x20));
        resealChunk(0);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName, kKey));

        std::vector<char> loaded(0x1000, 0);

        ut::CerrCapture cerr;
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_TRUE(cerr.contains("Failed to finalize AES-GCM decryption")) << cerr.text();

        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, ATruncatedTagIsRejected)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) } }, kKey);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName, kKey));

        // Losing the tail of the data file has to fail the read, not decrypt a
        // chunk whose tag is missing.
        repo.find(dataFile())->resize(0x800);

        std::vector<char> loaded(0x1000, 0);
        EXPECT_FALSE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));

        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, APartialReadOfAnEncryptedChunkReturnsTheRightSlice)
    {
        const auto original = ut::patternBuffer(0x1000, 0x1000);
        writeBackup(encryptedInfo(), { { 0, original } }, kKey);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName, kKey));

        std::vector<char> loaded(0x200, 0);
        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0x300));

        EXPECT_EQ(memcmp(loaded.data(), original.data() + 0x300, loaded.size()), 0);
        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, CompressionAndEncryptionTogetherRoundTrip)
    {
        const auto original = ut::patternBuffer(0x4000, 0x4000);
        writeBackup(encryptedInfo(9), { { 0, original } }, kKey);

        // The chunk is compressed first and encrypted after, so the stored chunk
        // is smaller than the original even with the tag added.
        const auto records = readRecords();
        ASSERT_EQ(records.size(), 1u);
        EXPECT_LT(records[0].chunk_length, records[0].orig_length);

        auto restore = newRestore();
        ASSERT_TRUE(restore->open(kPath, kName, kKey));

        std::vector<char> loaded(original.size(), 0);
        ASSERT_TRUE(restore->loadDataRange(0, loaded.data(), loaded.size(), 0));
        EXPECT_EQ(loaded, original);

        EXPECT_TRUE(restore->close());
    }

    TEST_F(EncryptionTest, EmptyBlocksAreStillEliminated)
    {
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x2000, 0x1000) } }, kKey);

        const auto records = readRecords();
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].orig_length, 0x1000u);
    }

    TEST_F(EncryptionTest, StoredSizeAccountsForTheBytesActuallyWritten)
    {
        // The counter has to include the authentication tag appended to every
        // chunk, otherwise it does not describe the data file it summarises.
        writeBackup(encryptedInfo(), { { 0, ut::patternBuffer(0x1000, 0x1000) },
                                       { 0x8000, ut::patternBuffer(0x1000, 0x1000) } },
                    kKey);

        EXPECT_EQ(readHeader().stored_data_size, repo.sizeOf(dataFile()));
    }
}
