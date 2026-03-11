#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path


def _parse_bool(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "t", "yes", "y", "on"}:
        return True
    if normalized in {"0", "false", "f", "no", "n", "off"}:
        return False
    raise argparse.ArgumentTypeError(f"invalid boolean value: {value}")


def _resolve_exe(build_dir: Path, name: str) -> Path:
    for c in (build_dir / "tests" / "integration" / name, build_dir / name):
        if c.exists():
            return c
    raise FileNotFoundError(f"cannot find executable {name} in {build_dir}")


def _run(cmd: list[str], timeout_sec: int = 15) -> tuple[int, str, str]:
    proc = subprocess.run(
        cmd,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout_sec,
    )
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def _last_json_line(text: str) -> dict:
    if not text:
        return {}
    return json.loads(text.splitlines()[-1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--evidence-dir", default=".sisyphus/evidence")
    parser.add_argument("--expect-detect", type=_parse_bool, default=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    build_dir = Path(args.build_dir).resolve()
    evidence_dir = Path(args.evidence_dir).resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)

    gate = _resolve_exe(build_dir, "kvstore_integrity_gate_test")
    corrupt_wal = repo_root / "scripts" / "integrity" / "corrupt_wal_byte.py"
    corrupt_sst = repo_root / "scripts" / "integrity" / "corrupt_sst_block.py"

    with tempfile.TemporaryDirectory(prefix="kvstore_task7_integrity_") as td:
        work = Path(td)
        wal_path = work / "wal" / "000001.wal"
        sst_wal_path = work / "sst" / "000001.wal"

        rc_make_wal, out_make_wal, err_make_wal = _run(
            [str(gate), "make_wal", str(wal_path)]
        )
        if rc_make_wal != 0:
            result = {
                "ts": datetime.now(timezone.utc).isoformat(),
                "pass": False,
                "reason": "make_wal_failed",
                "stdout": out_make_wal,
                "stderr": err_make_wal,
            }
            (evidence_dir / "integrity_corruption_suite.json").write_text(
                json.dumps(result, indent=2), encoding="utf-8"
            )
            return 1

        rc_corrupt_wal, out_corrupt_wal, err_corrupt_wal = _run(
            ["python3", str(corrupt_wal), "--wal", str(wal_path), "--offset", "24"]
        )
        rc_replay, out_replay, err_replay = _run(
            [str(gate), "replay_wal", str(wal_path)]
        )

        rc_make_sst, out_make_sst, err_make_sst = _run(
            [str(gate), "make_sst", str(sst_wal_path)]
        )
        sst_path = (
            Path(_last_json_line(out_make_sst).get("sst", ""))
            if out_make_sst
            else Path()
        )
        rc_corrupt_sst, out_corrupt_sst, err_corrupt_sst = _run(
            [
                "python3",
                str(corrupt_sst),
                "--sst",
                str(sst_path),
                "--block-index",
                "0",
                "--offset-in-block",
                "0",
            ]
        )
        rc_read_sst, out_read_sst, err_read_sst = _run(
            [str(gate), "read_sst", str(sst_path), "integrity-k"]
        )

        replay_payload = _last_json_line(out_replay)
        read_sst_payload = _last_json_line(out_read_sst)

        wal_detected = (
            rc_corrupt_wal == 0
            and rc_replay != 0
            and replay_payload.get("integrity_code") == "CHECKSUM_MISMATCH"
        )
        sst_detected = (
            rc_make_sst == 0
            and rc_corrupt_sst == 0
            and rc_read_sst != 0
            and read_sst_payload.get("integrity_code") == "CHECKSUM_MISMATCH"
        )
        detected = wal_detected and sst_detected

        result = {
            "ts": datetime.now(timezone.utc).isoformat(),
            "pass": detected == args.expect_detect,
            "wal_corruption": {
                "corrupt_cmd_exit": rc_corrupt_wal,
                "replay_exit": rc_replay,
                "replay_payload": replay_payload,
                "corrupt_stdout": out_corrupt_wal,
                "corrupt_stderr": err_corrupt_wal,
                "replay_stdout": out_replay,
                "replay_stderr": err_replay,
            },
            "sst_corruption": {
                "make_sst_exit": rc_make_sst,
                "corrupt_cmd_exit": rc_corrupt_sst,
                "read_exit": rc_read_sst,
                "read_payload": read_sst_payload,
                "make_sst_stdout": out_make_sst,
                "make_sst_stderr": err_make_sst,
                "corrupt_stdout": out_corrupt_sst,
                "corrupt_stderr": err_corrupt_sst,
                "read_stdout": out_read_sst,
                "read_stderr": err_read_sst,
            },
        }

    out = evidence_dir / "integrity_corruption_suite.json"
    out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
