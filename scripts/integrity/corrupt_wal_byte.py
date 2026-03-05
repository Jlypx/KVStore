#!/usr/bin/env python3
"""Deterministically corrupt one byte in a WAL file."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wal", required=True, help="Path to WAL file")
    parser.add_argument(
        "--offset",
        type=int,
        required=True,
        help="Zero-based byte offset to mutate",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    wal_path = Path(args.wal)
    if not wal_path.exists():
        raise FileNotFoundError(f"WAL file does not exist: {wal_path}")

    size = wal_path.stat().st_size
    if args.offset < 0 or args.offset >= size:
        raise ValueError(f"offset {args.offset} is outside WAL size {size}")

    with wal_path.open("r+b") as handle:
        handle.seek(args.offset)
        original = handle.read(1)
        if len(original) != 1:
            raise RuntimeError("failed to read target byte")

        mutated = bytes([original[0] ^ 0x01])
        handle.seek(args.offset)
        handle.write(mutated)
        handle.flush()

    print(
        f"corrupted wal={wal_path} offset={args.offset} "
        f"before=0x{original[0]:02x} after=0x{mutated[0]:02x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
