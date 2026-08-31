# abr-backup-lib

AltaStor Backup Library — a C++20 static library for creating and restoring
sector-level image backups, with pluggable storage backends (local
filesystem and S3-compatible object storage) and optional Zstd compression
and AES-256-GCM encryption.

## What it does

A backup is a byte range extracted from a raw image (e.g. a disk or volume),
split into fixed-size chunks. Chunks that are entirely zero are dropped
instead of stored, so sparse images only cost the space of the data they
actually contain. Each backup is made of two files, plus an optional
metadata blob:

- `<name>.abri` — the index: a fixed-size header (`IndexHeader`) followed
  by an array of `IndexRecord`s, one per stored chunk. It records where
  each chunk lives in the data file, its original offset/length in the
  image, and a CRC32C checksum.
- `<name>.abrd` — the data file: the (optionally compressed and/or
  encrypted) chunk bytes.
- `<name>.abrm` — optional free-form metadata associated with the backup.

The on-disk format is defined in [inc/abr_backup_format.h](inc/abr_backup_format.h).

A backup can be `full` or `incremental`; an incremental backup stores only
the blocks that changed since its parent and is not restorable on its own.

## Public API

The public interface lives in [inc/abr_backup.h](inc/abr_backup.h):

- `Abr::IBackupManager` — opens a storage location (a local folder or an
  S3 bucket) and manages the backups inside it:
  - `initialize(params)` — configure and open/create the storage location.
  - `listBackups(path, backups)` — list backup names under a path.
  - `createBackup(path, name, info, pBackup, encryptionKey = "")` —
    start a new backup, returning an `IBackup` to write into.
  - `openBackup(path, name, pRestore, decryptionKey = "")` — open an
    existing backup for restore, returning an `IRestore`.
  - `loadMetaData` / `saveMetaData` — read/write the metadata blob.
  - `deleteBackup(path, name)` — remove a backup and its files.
- `Abr::IBackup` — write side of a backup in progress:
  - `saveDataRange(buf, length, origOffset)` — write a chunk at a given
    offset in the original image.
  - `finalizeBackup(abort = false)` — finish (or discard) the backup.
- `Abr::IRestore` — read side of a finished backup:
  - `getDataRanges(ranges)` — enumerate the stored (non-zero) ranges.
  - `loadDataRange(rangeIndex, buf, length, offset)` — read data back.
  - `checkDataRange(rangeIndex, tempBuf)` — verify a chunk's checksum.
  - `close()` / `remove()`.
- `Abr::createBackupManager(mode, params)` — factory that builds an
  `IBackupManager`; `mode` is `"file"` ([inc/file_backup.h](inc/file_backup.h))
  or `"s3"` ([inc/s3_backup.h](inc/s3_backup.h)).

`BackupInfo` describes the image being backed up (sector size, physical
sector size, sector count, chunk alignment/size bounds, compression level,
encryption algorithm, full/incremental).

## Storage backends

- **File** (`FileBackupManager`) — stores backups under a local folder,
  which it locks down to `750` permissions (or stricter) and can create
  on demand.
- **S3** (`S3BackupManager`) — stores backups as objects in an S3-compatible
  bucket via the AWS SDK for C++. Supports a custom endpoint, region,
  credentials/profile, path-style addressing, and TLS verification
  toggling; can create the bucket if missing and cleans up any multipart
  uploads left behind by an interrupted run.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Dependencies: AWS SDK for C++ (`core`, `s3`), OpenSSL, Zstd, and Crc32c
(found via CMake packages where available, otherwise looked up as plain
libraries). Build options:

- `ABR_BACKUP_BUILD_TESTS` (default `ON`) — build the `btest` utility.
- `ABR_BACKUP_BUILD_UNIT_TESTS` (default `ON`) — build the GoogleTest unit
  tests, fetching GoogleTest automatically if it isn't installed.

## Tests

### Unit tests ([test/unit/](test/unit/))

GoogleTest-based tests that exercise the library's internals directly
(including the private `backup_core.h` core), registered with CTest via
`gtest_discover_tests`:

- `test_join_backup_path.cpp` — joining a path and a backup name.
- `test_index_format.cpp` — binary layout of `IndexHeader`/`IndexRecord`
  matches the documented on-disk format.
- `test_backup_core.cpp` — backup creation: parameter validation (sector
  size, alignment, compression level, encryption, name length), refusing
  to overwrite, and empty-block elimination.
- `test_restore_core.cpp` — opening a backup for restore: header/record
  validation, checksum checks, and reading data ranges back (including
  boundary and corruption cases).
- `test_compression.cpp` — Zstd compression round-trips at every
  supported level, shrinks compressible data, and detects corruption.
- `test_encryption.cpp` — AES-256-GCM round-trips, per-backup key
  material, authentication-tag verification, and tamper detection.
- `test_file_backup_manager.cpp` — the file backend: folder
  initialization/permissions, listing, backup lifecycle, and metadata.
- `test_backup_manager_factory.cpp` — `createBackupManager` mode
  dispatch and error handling.

Run them with:

```sh
ctest --test-dir build -L unit
```

### `btest` end-to-end utility ([test/btest/](test/btest/))

A small CLI (`btest`) that exercises a real `IBackupManager` against a
live folder or S3 bucket:

```
btest [options] <bucket/dir> <command> [parameters]
  Options:
    -f, --file     Use a local folder instead of an S3 bucket
    -c, --create   Create the folder/bucket when it is missing
  Commands:
    list [<path>]
    backup <path> <name>
    restore <path> <name>
    remove <path> <name>
    crypto <path> <name>
```

[scripts/tests.sh](scripts/tests.sh) drives `btest` through a black-box test
suite (repository setup/permissions, backup/list/restore, encryption,
removal, and failure cases) against a throwaway temporary folder for the
file backend, and optionally against S3:

```sh
scripts/tests.sh --build            # configure, build, and run
ABR_TEST_S3_BUCKET=my-bucket scripts/tests.sh   # also exercise the S3 backend
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
