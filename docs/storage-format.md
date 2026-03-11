# Storage Format (v1)

## Purpose

This document records the current implemented v1 on-disk storage formats.
It covers the write-ahead log, SSTable files, checksum coverage, corruption handling, operational limits, and the validation evidence that backs those claims.
It is intentionally narrow. It describes what exists in the repository today, not future storage features.

All integer fields in WAL and SST files are little-endian.
Both formats are explicitly versioned, and the current code only accepts version `1`.

## Storage layout in v1

At runtime, each `kvstore::engine::KvEngine` instance owns:

- one WAL file at the configured `wal_path`
- one sibling SST directory at `wal_path.parent_path() / "sst"`
- zero or more immutable SST files in that directory

Current storage behavior in `src/engine/kv_engine.cpp` is:

- writes append one WAL record per mutation before the memtable is updated
- `Flush()` writes a new SST file into the sibling `sst/` directory
- `Compact()` rewrites multiple SST files into one new SST, then removes the compacted input files
- SST filenames are numeric `.sst` files, padded to six digits while the id fits, for example `000001.sst`

The engine does not implement a manifest, bloom filters, compression, snapshots, or multi-version on-disk compatibility metadata.
The current durable state is just the configured WAL file plus the SST files that exist beside it.

## WAL v1 record format

WAL behavior is implemented in `include/kvstore/engine/wal.h` and `src/engine/wal.cpp`.

The WAL is a stream of back-to-back records.
There is no WAL file header, footer, or segment manifest in the current format.

### Header

Fixed 24 bytes:

| Field | Type | Value |
|---|---:|---|
| magic | u32 | `0x4b565741` |
| version | u16 | `1` |
| operation | u8 | `1 = Put`, `2 = Delete` |
| reserved | u8 | `0` |
| key_size | u32 | byte length of key payload |
| value_size | u32 | byte length of value payload |
| request_id_size | u32 | byte length of request id payload |
| crc32c | u32 | checksum over header without this field, plus payload |

### Payload

The payload immediately follows the 24 byte header:

```
u8 key[key_size]
u8 value[value_size]
u8 request_id[request_id_size]
```

For delete records, `operation = 2` and the value payload is empty in normal engine writes.

### Checksum coverage

WAL checksum coverage is exactly:

1. the first 20 bytes of the header, `magic`, `version`, `operation`, `reserved`, `key_size`, `value_size`, `request_id_size`
2. the full payload bytes, `key`, `value`, `request_id`

The stored `crc32c` field is not included in its own checksum.

### Reader validation and rejection rules

`WalReader::Replay()` rejects a record before invoking the replay callback when any of these checks fail:

- truncated 24 byte header
- WAL magic mismatch
- unsupported WAL version
- unknown operation tag
- record sizes above the v1 contract, `key > 1024`, `value > 1 MiB`, `request_id > 4 KiB`
- size arithmetic overflow while computing payload size
- truncated payload bytes
- checksum mismatch between stored and computed CRC32C

When a failure happens, replay returns an explicit `IntegrityError` such as `kTruncatedRecord`, `kInvalidMagic`, `kUnsupportedVersion`, `kInvalidOperation`, `kInvalidRecord`, or `kChecksumMismatch`.
The failing record is not accepted as valid state.

## SSTable v1 format

SST behavior is implemented in `include/kvstore/engine/sstable.h` and `src/engine/sstable.cpp`.

SSTables are immutable files containing sorted key/value entries, organized in blocks for efficient lookups and integrity verification.

### File layout

```
[FileHeader 16B]
[DataBlockFrame 0]
[DataBlockFrame 1]
...
[IndexBlockFrame]
[Footer 32B]
```

### Header

All integers are little-endian.

| Field | Type | Value |
|---|---:|---|
| magic | u32 | `0x4b535354` (`KSST`) |
| version | u16 | `1` |
| reserved | u16 | `0` |
| target_block_sz | u32 | writer hint, reader may ignore |
| reserved2 | u32 | `0` |

`KvEngine::Flush()` and `KvEngine::Compact()` currently write SSTs with `target_block_size = 4096`.
The writer also enforces a minimum effective block payload budget of `128` bytes by using `max(target_block_size, 128)`.

### Block framing and checksum coverage

Each data block and the index block use the same frame structure:

```
u32 payload_size
u8  payload[payload_size]
u32 crc32c
```

Checksum coverage is exactly the `payload` bytes.
It does not include `payload_size` or the trailing checksum field.

### Data block payload

Entries are encoded in ascending key order:

```
u32 key_size
u32 value_size
u8  key[key_size]
u8  value[value_size]
... repeated ...
```

`SstReader::Get()` scans only the selected block for an exact key match after index lookup.
`SstReader::ScanAll()` parses every data block and rejects malformed payload encoding.

### Index block payload

One entry per data block:

```
u32 block_count
repeat block_count times:
  u32 first_key_size
  u8  first_key[first_key_size]
  u64 block_offset
  u32 block_frame_size
```

Lookup strategy: binary search the index by `first_key` to find the candidate data block, then scan that block for the exact key.

The reader also verifies that `first_key` values are strictly increasing.
If index ordering is not monotonic, the SST is rejected as invalid.

### Footer

Fixed 32 bytes:

| Field | Type | Value |
|---|---:|---|
| footer_magic | u32 | `0x4b534654` (`KSFT`) |
| footer_version | u16 | `1` |
| reserved | u16 | `0` |
| index_offset | u64 | offset of the `IndexBlockFrame` |
| index_frame_size | u32 | total bytes of `IndexBlockFrame` |
| reserved2 | u32 | `0` |
| reserved3 | u32 | `0` |
| footer_crc32c | u32 | CRC32C over the first 28 bytes of the footer |

## Integrity rules and corruption handling

`docs/adr/0003-integrity-checksum-strategy-v1.md` sets the v1 rule: checksum verification is mandatory on WAL replay and SST read paths.

The current implementation enforces that rule this way.

### WAL integrity

- every record is checksummed when written in `WalWriter::Append()`
- replay recomputes CRC32C from the header fields except the checksum field, plus the full payload
- replay stops on the first structural or checksum failure
- corrupted bytes are surfaced as an explicit integrity error, not applied mutation state

### SST integrity

- `SstReader::Open()` verifies SST header magic and version
- the reader loads the fixed-size footer from the end of the file
- footer CRC32C must match the first 28 footer bytes before `index_offset` and `index_frame_size` are trusted
- the index frame must have a consistent `payload_size`, valid bounds, and a matching payload CRC32C
- each data block read verifies `payload_size`, reads the payload, then verifies the block CRC32C before parsing bytes into keys and values
- malformed block payload encoding also fails closed with `kInvalidRecord`

The current SST integrity model therefore covers:

- header magic and version
- footer magic, version, and footer checksum
- index frame bounds and checksum
- data block frame size consistency and checksum
- data block payload parsing bounds
- monotonic index ordering

## Operational assumptions and compatibility limits

This section describes the current v1 contract, not a future one.

- WAL and SST readers reject unsupported versions. There is no backward or forward multi-version negotiation in the repository today.
- WAL writes append to a single configured file with `std::ios::app`. WAL segmentation and rotation are not implemented here.
- The WAL durability contract assumes records are replayed in file order.
- SSTs are immutable once written. Compaction produces a new SST and removes older input SST files after a successful rewrite.
- SST readers ignore non-numeric filenames in the `sst/` directory and load numeric `.sst` files in ascending id order.
- Key, value, and request id sizes are constrained by the v1 engine contract, `key <= 1024`, `value <= 1 MiB`, `request_id <= 4 KiB`.
- `target_block_sz` is stored in the SST header as a writer hint. The reader does not rely on it for correctness.
- The current format does not claim compatibility with LevelDB, RocksDB, or any external storage format.
- The current format does not include manifests, bloom filters, compression, snapshots, tombstone-only block types, or multiple on-disk schema versions.

## Validation and evidence

### Code references used for this document

- `include/kvstore/engine/wal.h`
- `src/engine/wal.cpp`
- `include/kvstore/engine/sstable.h`
- `src/engine/sstable.cpp`
- `src/engine/kv_engine.cpp`
- `docs/adr/0003-integrity-checksum-strategy-v1.md`

### Corruption-detection evidence from Task 7

The concrete Task 7 integrity artifact is:

- `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`

That file records `"pass": true` for the corruption suite and preserves both failure-closed paths:

- WAL corruption:
  - `wal_corruption.replay_exit = 1`
  - `wal_corruption.replay_payload.integrity_code = "CHECKSUM_MISMATCH"`
  - `wal_corruption.replay_payload.message = "WAL checksum mismatch expected=3486638666 actual=817140712"`
- SST corruption:
  - `sst_corruption.read_exit = 1`
  - `sst_corruption.read_payload.integrity_code = "CHECKSUM_MISMATCH"`
  - `sst_corruption.read_payload.message = "sst=/tmp/kvstore_task7_integrity_mq4zhzrb/sst/sst/000001.sst SST block checksum mismatch expected=1103993438 actual=3822588266"`

Those results match the implemented behavior in `WalReader::Replay()` and `SstReader::ReadAndVerifyBlockFrame()`: corrupted persisted bytes are detected and surfaced as integrity failures before the engine trusts the record or block payload.
