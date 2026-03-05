# Storage Format (v1)

This document captures the minimal on-disk formats introduced by **Task 4**.

## SSTable v1 (KSST)

SSTables are immutable files containing sorted key/value entries, organized in
blocks for efficient lookups and integrity verification.

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

All integers are **little-endian**.

| Field | Type | Value |
|---|---:|---|
| magic | u32 | `0x4b535354` (`KSST`) |
| version | u16 | `1` |
| reserved | u16 | `0` |
| target_block_sz | u32 | writer hint (reader may ignore) |
| reserved2 | u32 | `0` |

### Block framing + checksum coverage

Each block is framed as:

```
u32 payload_size
u8  payload[payload_size]
u32 crc32c
```

Checksum coverage is **exactly the `payload` bytes** (does not include
`payload_size` nor the trailing checksum).

### Data block payload

Entries are encoded in ascending key order:

```
u32 key_size
u32 value_size
u8  key[key_size]
u8  value[value_size]
... repeated ...
```

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

Lookup strategy: binary search the index by `first_key` to find the candidate
data block, then scan that block for the exact key.

### Footer

Fixed 32 bytes:

| Field | Type | Value |
|---|---:|---|
| footer_magic | u32 | `0x4b534654` (`KSFT`) |
| footer_version | u16 | `1` |
| reserved | u16 | `0` |
| index_offset | u64 | offset of the IndexBlockFrame |
| index_frame_size | u32 | total bytes of IndexBlockFrame |
| reserved2 | u32 | `0` |
| reserved3 | u32 | `0` |
| footer_crc32c | u32 | crc32c over the first 28 bytes of the footer |
