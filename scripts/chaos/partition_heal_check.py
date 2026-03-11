#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import socket
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


def _resolve_exe(build_dir: Path, rel: str) -> Path:
    candidates = [
        build_dir / rel,
        build_dir / Path(rel).name,
    ]
    for c in candidates:
        if c.exists():
            return c
    raise FileNotFoundError(f"cannot find executable {rel} in {build_dir}")


def _run_json(
    cmd: list[str], timeout_sec: int, expect_zero: bool = True
) -> tuple[int, dict]:
    proc = subprocess.run(
        cmd,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout_sec,
    )
    payload = (
        json.loads(proc.stdout.strip().splitlines()[-1]) if proc.stdout.strip() else {}
    )
    if expect_zero and proc.returncode != 0:
        payload.setdefault("reason", "non_zero_exit")
    payload["exit_code"] = proc.returncode
    return proc.returncode, payload


def _pick_listen_addr() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", 0))
        host, port = sock.getsockname()
    return f"{host}:{port}"


def _wait_for_tcp_listen(
    proc: subprocess.Popen[str], target: str, timeout_sec: float
) -> bool:
    host, port = target.rsplit(":", 1)
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            return False
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(0.2)
            try:
                sock.connect((host, int(port)))
                return True
            except OSError:
                time.sleep(0.1)
    return False


def _stop_process(proc: subprocess.Popen[str]) -> dict:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    stdout, stderr = proc.communicate()
    return {
        "exit_code": proc.returncode,
        "stdout": stdout.strip(),
        "stderr": stderr.strip(),
    }


def _run_tls_profile_smoke(
    kvd_exe: Path, tls_exe: Path, tls_profile: str, cert: Path, key: Path
) -> dict:
    target = _pick_listen_addr()
    smoke_cmd = [
        str(tls_exe),
        "--mode=external",
        f"--target={target}",
        f"--tls_profile={tls_profile}",
    ]
    kvd_cmd = [
        str(kvd_exe),
        f"--listen_addr={target}",
        f"--tls_profile={tls_profile}",
    ]
    if tls_profile == "secure":
        kvd_cmd.extend([f"--tls_cert={cert}", f"--tls_key={key}"])
        smoke_cmd.append(f"--tls_cert={cert}")

    with tempfile.TemporaryDirectory(prefix=f"kvd-{tls_profile}-") as data_dir:
        kvd_cmd.append(f"--data_dir={data_dir}")
        proc = subprocess.Popen(
            kvd_cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        smoke_code = 1
        smoke_payload: dict = {}
        reason: str | None = None
        try:
            if not _wait_for_tcp_listen(proc, target, 5):
                reason = f"kvd_not_ready:{target}"
            else:
                smoke_code, smoke_payload = _run_json(smoke_cmd, 25)
        finally:
            kvd_payload = _stop_process(proc)

    passed = smoke_code == 0 and smoke_payload.get("pass") is True
    payload = {
        "target": target,
        "tls_profile": tls_profile,
        "kvd_cmd": kvd_cmd,
        "smoke_cmd": smoke_cmd,
        "kvd": kvd_payload,
        "smoke": smoke_payload,
        "pass": passed,
    }
    if reason is not None:
        payload["reason"] = reason
    return payload


def _write_payload(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--evidence-dir", default=".sisyphus/evidence")
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    repo_root = Path(args.repo_root).resolve()
    evidence_dir = Path(args.evidence_dir).resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)

    chaos_exe = _resolve_exe(build_dir, "tests/integration/kvstore_chaos_gate_test")
    kvd_exe = _resolve_exe(build_dir, "src/kvd")
    tls_exe = _resolve_exe(build_dir, "tests/grpc/kvstore_tls_profile_toggle_test")
    cert = repo_root / "tests" / "assets" / "tls" / "server.crt"
    key = repo_root / "tests" / "assets" / "tls" / "server.key"

    chaos_code, chaos_payload = _run_json([str(chaos_exe), "partition_heal"], 20)
    dev_payload = _run_tls_profile_smoke(kvd_exe, tls_exe, "dev", cert, key)
    secure_payload = _run_tls_profile_smoke(kvd_exe, tls_exe, "secure", cert, key)
    tls_payload = {
        "profiles": {
            "dev": dev_payload,
            "secure": secure_payload,
        },
        "pass": dev_payload.get("pass") is True and secure_payload.get("pass") is True,
    }

    result = {
        "ts": datetime.now(timezone.utc).isoformat(),
        "partition_heal": chaos_payload,
        "tls_profile_toggle": tls_payload,
        "pass": chaos_code == 0
        and chaos_payload.get("pass") is True
        and tls_payload.get("pass") is True,
    }

    _write_payload(evidence_dir / "task-7-partition-heal.log", chaos_payload)
    _write_payload(evidence_dir / "task-7-tls-toggle.log", tls_payload)

    out = evidence_dir / "chaos_partition_heal_and_tls.json"
    _write_payload(out, result)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
