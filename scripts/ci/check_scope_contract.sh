#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROTO_FILE="${ROOT_DIR}/proto/kvstore/v1/kv.proto"

if [[ ! -f "${PROTO_FILE}" ]]; then
  echo "[scope-contract] ERROR: missing proto file: ${PROTO_FILE}" >&2
  exit 1
fi

raw_rpc_count="$(grep -Ec '^[[:space:]]*rpc[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' "${PROTO_FILE}")"

if [[ "${raw_rpc_count}" -ne 3 ]]; then
  echo "[scope-contract] ERROR: expected exactly 3 RPC declarations, found ${raw_rpc_count}" >&2
  exit 1
fi

mapfile -t rpc_names < <(
  grep -E '^[[:space:]]*rpc[[:space:]]+[A-Za-z_][A-Za-z0-9_]*' "${PROTO_FILE}" \
    | sed -E 's/^[[:space:]]*rpc[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/' \
    | sort -u
)

expected=(Delete Get Put)

if [[ "${#rpc_names[@]}" -ne 3 ]]; then
  echo "[scope-contract] ERROR: expected exactly 3 distinct RPCs (Put/Get/Delete), found ${#rpc_names[@]}" >&2
  printf '[scope-contract] Found RPCs: %s\n' "${rpc_names[*]:-<none>}" >&2
  exit 1
fi

for i in "${!expected[@]}"; do
  if [[ "${rpc_names[$i]}" != "${expected[$i]}" ]]; then
    echo "[scope-contract] ERROR: RPC set mismatch. Expected: ${expected[*]}, Found: ${rpc_names[*]}" >&2
    exit 1
  fi
done

if grep -Eq '^[[:space:]]*rpc[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\([[:space:]]*stream\b' "${PROTO_FILE}"; then
  echo "[scope-contract] ERROR: request streaming RPC detected in v1 (not allowed)" >&2
  exit 1
fi

if grep -Eq '^[[:space:]]*rpc[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[^;]*returns[[:space:]]*\([[:space:]]*stream\b' "${PROTO_FILE}"; then
  echo "[scope-contract] ERROR: response streaming RPC detected in v1 (not allowed)" >&2
  exit 1
fi

if grep -Eq '^[[:space:]]*rpc[[:space:]]*(Txn|Watch|Lease|RangeScan|Range|DeleteRange)\b' "${PROTO_FILE}"; then
  echo "[scope-contract] ERROR: forbidden v1 RPC detected (Txn/Watch/Lease/Range*)" >&2
  exit 1
fi

echo "[scope-contract] OK: v1 proto contract contains unary Put/Get/Delete only"
