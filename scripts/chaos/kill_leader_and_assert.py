#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from datetime import datetime, timezone


def _resolve_exe(build_dir: Path, name: str) -> Path:
    candidates = [
        build_dir / "tests" / "integration" / name,
        build_dir / name,
    ]
    for c in candidates:
        if c.exists():
            return c
    raise FileNotFoundError(f"cannot find executable {name} in {build_dir}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--evidence-dir", default=".sisyphus/evidence")
    parser.add_argument(
        "--timeout-sec", "--max-election-sec", dest="timeout_sec", type=int, default=15
    )
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    evidence_dir = Path(args.evidence_dir).resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)

    exe = _resolve_exe(build_dir, "kvstore_chaos_gate_test")
    proc = subprocess.run(
        [str(exe), "failover"],
        check=False,
        capture_output=True,
        text=True,
        timeout=args.timeout_sec,
    )
    stdout = proc.stdout.strip()
    payload = json.loads(stdout.splitlines()[-1]) if stdout else {}
    payload["exit_code"] = proc.returncode
    payload["ts"] = datetime.now(timezone.utc).isoformat()

    out = evidence_dir / "chaos_kill_leader_and_assert.json"
    out.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    return 0 if proc.returncode == 0 and payload.get("pass") is True else 1


if __name__ == "__main__":
    raise SystemExit(main())
