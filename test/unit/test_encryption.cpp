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
#include <string>
#include <vector>

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
