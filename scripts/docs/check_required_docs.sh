#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FAILURES=0

check_file() {
  local rel_path="$1"
  local abs_path="${ROOT_DIR}/${rel_path}"

  if [[ ! -f "${abs_path}" ]]; then
    printf '[required-docs] ERROR: missing file: %s\n' "${rel_path}" >&2
    FAILURES=1
  fi
}

check_marker() {
  local rel_path="$1"
  local marker="$2"
  local abs_path="${ROOT_DIR}/${rel_path}"

  if [[ ! -f "${abs_path}" ]]; then
    return
  fi

  if ! grep -Fq "${marker}" "${abs_path}"; then
    printf '[required-docs] ERROR: missing heading "%s" in %s\n' "${marker}" "${rel_path}" >&2
    FAILURES=1
  fi
}

check_file "docs/architecture.md"
check_marker "docs/architecture.md" "## Purpose"
check_marker "docs/architecture.md" "## System Overview"
check_marker "docs/architecture.md" "## Validation and Evidence"

check_file "docs/wire-protocol.md"
check_marker "docs/wire-protocol.md" "## Purpose"
check_marker "docs/wire-protocol.md" "## Service and RPC inventory"
check_marker "docs/wire-protocol.md" "## Validation and evidence"

check_file "docs/storage-format.md"
check_marker "docs/storage-format.md" "## Purpose"
check_marker "docs/storage-format.md" "## WAL v1 record format"
check_marker "docs/storage-format.md" "## Validation and evidence"

check_file "docs/operations.md"
check_marker "docs/operations.md" "## Purpose"
check_marker "docs/operations.md" "## Runtime model in v1"
check_marker "docs/operations.md" "## Evidence-backed validation commands"

check_file "docs/testing.md"
check_marker "docs/testing.md" "## Purpose"
check_marker "docs/testing.md" "## Test layers / strategy"
check_marker "docs/testing.md" "## Evidence and reproducibility"

check_file "docs/security.md"
check_marker "docs/security.md" "## Purpose"
check_marker "docs/security.md" "## Transport security profiles"
check_marker "docs/security.md" "## Explicit gaps / non-goals"

check_file "docs/release.md"
check_marker "docs/release.md" "## Purpose"
check_marker "docs/release.md" "## Required build/test gates"
check_marker "docs/release.md" "## Validation and references"

if [[ "${FAILURES}" -ne 0 ]]; then
  exit 1
fi

printf '[required-docs] OK: required Task 8 docs and core headings are present\n'
