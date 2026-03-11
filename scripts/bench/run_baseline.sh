#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_DIR_SET=0
EVIDENCE_DIR="${EVIDENCE_DIR:-${ROOT_DIR}/.sisyphus/evidence}"
OUT_JSON="${EVIDENCE_DIR}/bench_baseline.json"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      if [[ $# -lt 2 ]]; then
        echo "missing value for --out" >&2
        exit 1
      fi
      OUT_JSON="$2"
      shift 2
      ;;
    --*)
      echo "unknown option: $1" >&2
      exit 1
      ;;
    *)
      if [[ ${BUILD_DIR_SET} -eq 1 ]]; then
        echo "unexpected argument: $1" >&2
        exit 1
      fi
      BUILD_DIR="$1"
      BUILD_DIR_SET=1
      shift
      ;;
  esac
done

mkdir -p "${EVIDENCE_DIR}"
mkdir -p "$(dirname "${OUT_JSON}")"

BENCH_BIN="${BUILD_DIR}/tests/integration/kvstore_bench_gate_test"
if [[ ! -x "${BENCH_BIN}" ]]; then
  BENCH_BIN="${BUILD_DIR}/kvstore_bench_gate_test"
fi
if [[ ! -x "${BENCH_BIN}" ]]; then
  echo "benchmark binary not found: kvstore_bench_gate_test" >&2
  exit 1
fi

"${BENCH_BIN}" >"${OUT_JSON}"
python3 "${ROOT_DIR}/scripts/bench/assert_slo.py" --input "${OUT_JSON}" --output "${EVIDENCE_DIR}/bench_slo_assertion.json"

echo "benchmark evidence: ${OUT_JSON}"
