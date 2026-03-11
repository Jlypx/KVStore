#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="Path to benchmark JSON")
    parser.add_argument("--output", help="Path to assertion JSON")
    parser.add_argument(
        "--max-write-p99-ms",
        "--p99-write-ms",
        dest="max_write_p99_ms",
        type=float,
        default=20.0,
    )
    parser.add_argument(
        "--max-read-p99-ms",
        "--p99-read-ms",
        dest="max_read_p99_ms",
        type=float,
        default=10.0,
    )
    args = parser.parse_args()

    payload = json.loads(Path(args.input).read_text(encoding="utf-8"))

    write_p99 = float(payload.get("p99_durable_write_ms", float("inf")))
    read_p99 = float(payload.get("p99_read_ms", float("inf")))
    no_loss = bool(payload.get("no_acknowledged_write_loss", False))

    result = {
        "ts": datetime.now(timezone.utc).isoformat(),
        "input": str(Path(args.input).resolve()),
        "thresholds": {
            "max_write_p99_ms": args.max_write_p99_ms,
            "max_read_p99_ms": args.max_read_p99_ms,
            "no_acknowledged_write_loss": True,
        },
        "observed": {
            "p99_durable_write_ms": write_p99,
            "p99_read_ms": read_p99,
            "no_acknowledged_write_loss": no_loss,
        },
        "pass": write_p99 <= args.max_write_p99_ms
        and read_p99 <= args.max_read_p99_ms
        and no_loss,
    }

    if args.output:
        Path(args.output).write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
