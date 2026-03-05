#!/usr/bin/env python3
"""Deterministically corrupt one byte in an SSTable data block payload.

This helper is intentionally small and matches the SSTable v1 layout documented
in docs/storage-format.md.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


SST_HEADER_SIZE = 16
SST_FOOTER_SIZE = 32
FOOTER_MAGIC = 0x4B534654  # 'KSFT'


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sst", required=True, help="Path to SST file")
    parser.add_argument(
        "--block-index",
        type=int,
        required=True,
        help="Zero-based data block index (does not count index block)",
    )
    parser.add_argument(
        "--offset-in-block",
        type=int,
        default=0,
        help="Zero-based byte offset within the block payload to mutate",
    )
    parser.add_argument(
        "--mask",
        type=lambda s: int(s, 0),
        default=0x01,
        help="XOR mask to apply to the target byte (default: 0x01)",
    )
    return parser.parse_args()


def read_footer(handle) -> tuple[int, int]:
    handle.seek(0, 2)
    size = handle.tell()
    if size < SST_HEADER_SIZE + SST_FOOTER_SIZE:
        raise ValueError(f"SST file too small: size={size}")
    handle.seek(size - SST_FOOTER_SIZE)
    footer = handle.read(SST_FOOTER_SIZE)
    if len(footer) != SST_FOOTER_SIZE:
        raise RuntimeError("failed to read SST footer")

    footer_magic, footer_version, _reserved = struct.unpack_from("<IHH", footer, 0)
    if footer_magic != FOOTER_MAGIC:
        raise ValueError(f"footer magic mismatch: 0x{footer_magic:08x}")
    if footer_version != 1:
        raise ValueError(f"unsupported footer version: {footer_version}")

    index_offset = struct.unpack_from("<Q", footer, 8)[0]
    index_frame_size = struct.unpack_from("<I", footer, 16)[0]
    # We don't verify footer checksum here; the engine reader will.
    return index_offset, index_frame_size


def find_data_block_offset(handle, index_offset: int, block_index: int) -> tuple[int, int]:
    if block_index < 0:
        raise ValueError("block-index must be >= 0")

    offset = SST_HEADER_SIZE
    current = 0
    while offset < index_offset:
        handle.seek(offset)
        size_bytes = handle.read(4)
        if len(size_bytes) != 4:
            raise RuntimeError("failed to read block payload size")
        payload_size = struct.unpack("<I", size_bytes)[0]
        frame_size = 4 + payload_size + 4
        if offset + frame_size > index_offset:
            raise ValueError("block frame crosses index offset; file is malformed")
        if current == block_index:
            return offset, payload_size
        offset += frame_size
        current += 1

    raise IndexError(
        f"data block index out of range: requested={block_index} count={current}"
    )


def main() -> int:
    args = parse_args()
    sst_path = Path(args.sst)
    if not sst_path.exists():
        raise FileNotFoundError(f"SST file does not exist: {sst_path}")

    with sst_path.open("r+b") as handle:
        index_offset, _index_frame_size = read_footer(handle)
        block_offset, payload_size = find_data_block_offset(
            handle, index_offset=index_offset, block_index=args.block_index
        )

        if args.offset_in_block < 0 or args.offset_in_block >= payload_size:
            raise ValueError(
                f"offset-in-block {args.offset_in_block} outside payload_size {payload_size}"
            )

        target = block_offset + 4 + args.offset_in_block
        handle.seek(target)
        original = handle.read(1)
        if len(original) != 1:
            raise RuntimeError("failed to read target byte")
        mutated = bytes([original[0] ^ (args.mask & 0xFF)])
        handle.seek(target)
        handle.write(mutated)
        handle.flush()

    print(
        "corrupted sst={} block_index={} offset_in_block={} file_offset={} before=0x{:02x} after=0x{:02x}".format(
            sst_path,
            args.block_index,
            args.offset_in_block,
            target,
            original[0],
            mutated[0],
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
