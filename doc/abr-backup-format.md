# ABR Backup Format v1.0

This document describes version 1.0 of the `.abr` backup format.
That is the format this library (`backuplib`) writes and reads, and the one `abr-vmdk-tool` and `abr-vhdx-tool`
store their backups in.

## Contents

- [Overview](#overview)
- [Repository layout](#repository-layout)
- [Index file (.abri)](#index-file-abri)
  - [IndexHeader (4096 bytes)](#indexheader-4096-bytes)
  - [IndexRecord (32 bytes)](#indexrecord-32-bytes)
- [Data file (.abrd)](#data-file-abrd)
  - [Chunking and zero-block elimination](#chunking-and-zero-block-elimination)
- [Encryption](#encryption)
- [Integrity](#integrity)
- [Full and incremental backups](#full-and-incremental-backups)
- [Write and commit protocol](#write-and-commit-protocol)
- [Metadata (.abrm)](#metadata-abrm)
- [Immutability (S3 Object Lock)](#immutability-s3-object-lock)
- [Reading a backup](#reading-a-backup)
- [Limitations and compatibility notes](#limitations-and-compatibility-notes)
- [Source references](#source-references)

## Overview

An ABR backup is a sector-level image of one disk.
The image is split into chunks, and all-zero chunks are left out.
Each chunk that remains is stored with a CRC32C checksum, optionally Zstd-compressed and AES-256-GCM-encrypted.
One backup is made of up to three objects that share a base name:

| Object | Required | Contents |
| --- | --- | --- |
| `<name>.abri` | yes | **Index.** A fixed 4096-byte header followed by one 32-byte record per stored chunk. It says where each chunk sits in the data file and where it belongs in the disk image. The index is what makes a backup exist. |
| `<name>.abrd` | yes | **Data.** The stored chunk bytes, written back to back with no framing, padding or header. |
| `<name>.abrm` | no | **Metadata.** An opaque blob that the format never interprets, for example the JSON manifest a backup server keeps about the disk. |

A backup is either **full**, holding the whole disk, or **incremental**, holding only the blocks that changed since its
parent.
An incremental cannot be restored on its own.

## Repository layout

A repository is a local folder (`file` backend) or an S3 bucket (`s3` backend).
Backups live under an optional sub-path, so each object of a backup is found at `<path>/<name><ext>`, or at
`<name><ext>` when the path is empty.
On S3 that string is the object key.

```text
<repository>/
  <path>/
    web01-sda.abri        index
    web01-sda.abrd        data
    web01-sda.abrm        metadata (optional)
    web01-sda-inc1.abri
    web01-sda-inc1.abrd
```

- **Listing.** A backup is listed when its `.abri` exists.
  A data file with no index is not a backup: it is the leftover of a run that died before it committed.
- **Naming.** The base name plus `.abrd` must fit in 3519 bytes, the size of the name list in the index header.
  Longer names are refused when the backup is created.
- **No overwrite.** Creating a backup fails if its index already exists, on both backends.
- **File backend permissions (POSIX).** Folders are created as `0750` and files as `0640`.
  An existing repository folder more permissive than `0750` is refused, or tightened when the caller asked for the
  folder to be set up.
  On Windows the folder ACL decides access instead.
- **Delete.** Deleting a backup removes `.abrd`, `.abri` and `.abrm` by name.
  It does not need a readable index, so it also cleans up the leftovers of an interrupted run.
  On S3 it also aborts any pending multipart upload of the data object.

## Index file (.abri)

All integers are **little-endian**.
The structures are written as their in-memory image on x86-64 (GCC and MinGW), with natural alignment, and the
library refuses to build on a big-endian host.
The offsets below are the on-disk contract.

```text
offset 0       +------------------------------+
               | IndexHeader       4096 bytes |
offset 4096    +------------------------------+  <- index_offset
               | IndexRecord[0]      32 bytes |
               | IndexRecord[1]      32 bytes |
               | ...                          |
               | IndexRecord[index_count - 1] |
               +------------------------------+  file size = 4096 + 32 * index_count
```

### IndexHeader (4096 bytes)

| Offset | Size | Field | Description |
| ---: | ---: | --- | --- |
| 0 | 8 | `signature` | ASCII `abrindex`, not NUL-terminated. |
| 8 | 2 | `version` | Format version, `1`. Readers reject any other value. |
| 10 | 1 | `compression` | `0` means raw. `1` to `22` means Zstd at that level. The level is recorded for information only, since decompression does not need it. The header comment says 1 to 9, but the code accepts the whole Zstd range. |
| 11 | 1 | `encryption` | `0` means none. `1` means AES-256-GCM. Readers reject other values. |
| 12 | 4 | `header_crc32c` | CRC32C of all 4096 header bytes, computed with this field set to zero. Readers always check it. |
| 16 | 8 | `image_size` | Size of the original disk in bytes, which is `sectors × sector_size`. Readers get the sector count by dividing it by `sector_size`. |
| 24 | 2 | `sector_size` | Logical sector size, `512` or `4096`. Readers reject anything else. |
| 26 | 2 | `phy_sector_size` | Physical sector size, a non-zero multiple of `sector_size`. |
| 28 | 4 | `index_count` | Number of index records. It can be `0`, for an empty disk or a no-change incremental. |
| 32 | 4 | `index_offset` | Offset of the first record from the start of the file. Always `4096` today, and readers seek to it. |
| 36 | 4 | `index_crc32c` | CRC32C over all `32 × index_count` record bytes. `0` when there are no records. Readers check it whenever there are records. |
| 40 | 4 | `min_data_block` | Minimum data block size the writer was given. `0` means no minimum. It raises the zero-block detection granularity, described below. |
| 44 | 4 | `max_data_block` | Maximum original length of a chunk. `0` means no limit. Restore uses it as its read buffer size, falling back to 4 MiB when it is zero. |
| 48 | 4 | `data_file_names_offset` | Offset of `data_file_names` from the start of the header, which is `576`. |
| 52 | 1 | flags | Bit 0 is `incremental`: `0` means full, `1` means incremental. Bits 1 to 7 are reserved and must be zero. |
| 53 | 11 | `reserved` | Must be zero. |
| 64 | 32 | `encryption_key` | The wrapped data encryption key, when `encryption = 1`. Otherwise zero. See [Encryption](#encryption). |
| 96 | 16 | `encryption_iv` | Random IV of this backup, when `encryption = 1`. Otherwise zero. |
| 112 | 8 | `orig_data_size` | Sum of `orig_length` over all records, which is the bytes of data the backup holds. Statistics only. |
| 120 | 8 | `stored_data_size` | Sum of `chunk_length` over all records, which equals the size of the data file. It counts the bytes after compression and encryption, GCM tags included. Statistics only. |
| 128 | 448 | `reserved1` | Must be zero. |
| 576 | 3520 | `data_file_names` | List of NUL-terminated UTF-8 data file names. Today it always holds one entry, `<name>.abrd`, and the rest is zero. |

### IndexRecord (32 bytes)

One record describes one stored chunk.
Records appear in the order the chunks were written, so `chunk_offset` increases and the chunks tile the data file
without gaps.
The tools write in increasing disk offset order, but readers do not depend on that.

| Offset | Size | Field | Description |
| ---: | ---: | --- | --- |
| 0 | 8 | `chunk_offset` | Byte offset of the chunk in the data file. |
| 8 | 4 | `chunk_length` | Stored length of the chunk in the data file, after compression and encryption. It includes the 16-byte GCM tag when the backup is encrypted. |
| 12 | 4 | `chunk_crc32c` | CRC32C of the stored bytes, exactly as they are in the data file. Readers always check it. |
| 16 | 8 | `orig_offset` | Byte offset of the data in the original disk image. |
| 24 | 4 | `orig_length` | Length of the data in the original image, before compression. A chunk therefore covers less than 4 GiB of the disk. |
| 28 | 1 | `file_id` | Index into `data_file_names`. Always `0`; readers refuse a backup with any other value. |
| 29 | 3 | `reserved` | Must be zero. |

A range of the disk that no record covers reads back as zeros in a full backup, and as *unchanged* in an incremental
one.

## Data file (.abrd)

The data file is the plain concatenation of the stored chunks.
It has no header, no magic and no per-chunk framing; every boundary comes from the index.
An empty backup has an empty data file.

Each stored chunk is produced from `orig_length` bytes of the disk in this order:

```text
disk bytes [orig_offset, orig_offset + orig_length)
   |
   |  compression != 0:  one independent Zstd frame (ZSTD_compress at the header's level)
   v
compressed bytes
   |
   |  encryption == 1:   AES-256-GCM, ciphertext followed by a 16-byte tag
   v
stored bytes  -- chunk_length bytes at chunk_offset in .abrd
   |
   '-- chunk_crc32c = CRC32C(stored bytes)
```

Because every chunk is self-contained, any chunk can be read, checked and decoded independently of the others.

### Chunking and zero-block elimination

The writer receives the disk as a sequence of buffers, each with its disk offset.
The tools read only the allocated ranges the source reports and pass at most 4 MiB at a time, rounded to whole
sectors.
Each buffer is cut into chunks like this:

1. The detection block size is `max(phy_sector_size, 4096, min_data_block)`.
2. **Full backups only:** leading whole blocks that are entirely zero are skipped.
   No record and no data are written for them.
3. The next run of blocks that hold any non-zero byte becomes the chunk, capped at `max_data_block` when that is set.
4. A tail shorter than one block that is all zero is dropped as well.

An **incremental** backup stores every byte it is given, zeros included.
In an incremental a missing block means "unchanged", so a block that changed from data to zeros has to be stored, or
the parent's stale data would survive a restore.

## Encryption

Encryption is per backup and uses a random data encryption key (DEK) that is wrapped with a key derived from the
user's secret (`--key`).

### At backup creation

1. Generate a random 32-byte **DEK** and a random 16-byte **IV** (OpenSSL `RAND_bytes`).
2. Derive the key encryption key: `KEK = SHA-256(secret)`, where the secret is taken as raw bytes.
3. Wrap the key: `encryption_key = AES-256-CTR(KEK, IV, DEK)`, a 32-byte ciphertext with no tag.
4. Store `encryption_key` and `IV` in the index header.

### Per chunk

- Cipher: AES-256-GCM with the DEK.
  No additional authenticated data is used.
- Nonce: 12 bytes, the standard GCM nonce length, which writer and reader set explicitly.
  It is bytes 0 to 11 of the header IV, with `orig_offset` XORed into bytes 0 to 7 as little-endian.
  Bytes 12 to 15 of the IV are not part of the nonce.
- Output: ciphertext, which is as long as the plaintext, followed by the 16-byte tag.
  `chunk_length` includes the tag.

Nonces are unique within a backup because each disk offset is stored at most once.
Across backups they are unique because every backup has its own DEK.

### On restore

The reader computes the KEK from the secret it was given, unwraps the DEK with AES-256-CTR, and decrypts each chunk,
which checks its GCM tag.
The format holds no key check value, so a wrong secret yields a wrong DEK without any error at unwrap time.
The tools therefore decrypt the first chunk up front and refuse a key that fails it, before any target is opened.

## Integrity

| Check | Covers | When it runs |
| --- | --- | --- |
| `header_crc32c` | The 4096-byte header | When the backup is opened |
| `index_crc32c` | All index records | When the backup is opened, if it has records |
| `chunk_crc32c` | Stored chunk bytes | On every chunk read, and on `verify`, which does not decrypt |
| GCM tag | Chunk ciphertext, for authenticity | On every decrypt |
| Zstd size check | The decompressed length must equal `orig_length` | On every decompress |

All checksums are CRC32C (Castagnoli).
Readers check every one of them; a stored `0` is checked like any other value, never read as "not recorded".
The CRCs detect accidental corruption only.
The index carries no MAC, so only the chunk contents are authenticated.

## Full and incremental backups

- A **full** backup describes the whole disk. Uncovered ranges are zeros.
- An **incremental** backup has the `incremental` bit set and holds only changed blocks, from VMware CBT or Hyper-V
  RCT. Uncovered ranges are unchanged.
- **No parent link.** The format records nothing that ties an incremental to its parent: no parent name, no change id,
  no image identity.
  The chain is whatever order the caller supplies.
  Restore applies the full backup and then each incremental onto the same target with `--force`.
  `copy --source-names` merges a chain into a new full backup, with later backups winning where they overlap.
- What the tools can check is that the first backup in a chain is full, that every later one is incremental, and that
  all of them have the same `sector_size` and sector count.

## Write and commit protocol

A backup becomes visible atomically, through its index:

1. Create the index object, failing if it already exists, and the data object.
   On S3 the data object is a multipart upload.
2. Stream chunks into the data object.
   On S3 they are buffered into parts of 5 to 8 MB.
3. Close the data object.
   On S3 this completes the multipart upload.
4. Only then write the whole index, header and records, in one write, and close it.
   On S3 this is a single `PutObject`.

Any failure, abort or interruption before step 4 completes removes what was written, index first, so a listed backup
is never one that cannot be restored.
If even that cleanup fails, the library reports that the partial backup has to be deleted by hand.

## Metadata (.abrm)

The metadata blob is stored and returned byte for byte through `meta_save` and `meta_load`.
It is written separately from the backup, has no header or checksum, is not encrypted, and is not carried over by
`copy`.
Consumers that store JSON in it own that schema.

## Immutability (S3 Object Lock)

Retention is a property of the storage, not of the format: the index header has no retention field.
On a bucket with Object Lock, `.abrd` and `.abri` are written with the requested mode, `GOVERNANCE` or `COMPLIANCE`,
and a retain-until date.
`retention` later applies the same hold to all three objects, including `.abrm` when it exists.
Readers report the retention found on the `.abri` object as the backup's retention.

## Reading a backup

1. Read the first 4096 bytes of `<name>.abri`.
   Check the signature, `version == 1` and `header_crc32c`.
2. Validate `sector_size` (512 or 4096), `compression` (22 or less) and `encryption` (1 or less).
   If the backup is encrypted, unwrap the DEK.
3. Read `index_count × 32` bytes at `index_offset`, and check `index_crc32c` when there are records.
   Refuse the backup if any record has a `file_id` other than `0`.
4. Open `<name>.abrd`.
   The reader derives this name from the base name and does not consult `data_file_names`.
5. For each record, read `chunk_length` bytes at `chunk_offset`, check `chunk_crc32c`, decrypt, decompress, and write
   `orig_length` bytes at `orig_offset` on the target.

## Limitations and compatibility notes

- **Byte order and layout** are the x86-64 little-endian struct image, and the library refuses to build on a
  big-endian host.
  A big-endian or differently packed implementation must serialise field by field at the offsets above.
- **The GCM nonce is 12 bytes.**
  Bytes 12 to 15 of `encryption_iv` take part in key wrapping, which uses the full 16-byte CTR counter, but not in
  chunk encryption.
- **Key derivation is a single unsalted SHA-256** of the secret, with no work factor.
  The secret should be a high-entropy key rather than a human password.
- **Multi-file support is reserved but unused.**
  `file_id`, the `data_file_names` list and `INDEX_FILE_MAX_FILES = 256` anticipate a backup split across several
  data files.
  Writers always produce one data file; readers open only that one and refuse records with a non-zero `file_id`.
- **Write alignment is not recorded.**
  `BackupInfo.alignment` is validated at creation (1 MiB or less) but is not stored in the header and does not affect
  the layout.
- **Versioning.** Readers accept exactly version 1.
  Any change to the layout or the meaning of an existing field needs a new version number.
  New optional data can use the reserved areas, which writers zero today.

## Source references

- [inc/abr_backup_format.h](../inc/abr_backup_format.h): the on-disk structures and constants
- [src/backup_core.cpp](../src/backup_core.cpp): chunking, compression, encryption, the commit protocol and the reader
- [src/file_backup.cpp](../src/file_backup.cpp), [src/s3_backup.cpp](../src/s3_backup.cpp): the storage backends
- [test/unit/test_index_format.cpp](../test/unit/test_index_format.cpp): tests that pin the binary layout
- [test/unit/test_encryption.cpp](../test/unit/test_encryption.cpp): tests that pin the key and nonce derivation
