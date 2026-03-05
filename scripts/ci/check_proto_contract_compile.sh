#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROTO_FILE="${ROOT_DIR}/proto/kvstore/v1/kv.proto"
GEN_DIR="${ROOT_DIR}/build/gen"
LOCAL_TOOLS_SYSROOT="${ROOT_DIR}/.tools/proto/sysroot"
LOCAL_PROTOC="${LOCAL_TOOLS_SYSROOT}/usr/bin/protoc"
LOCAL_GRPC_PLUGIN="${LOCAL_TOOLS_SYSROOT}/usr/bin/grpc_cpp_plugin"

"${ROOT_DIR}/scripts/ci/check_scope_contract.sh"

if [[ ! -f "${PROTO_FILE}" ]]; then
  echo "[proto-compile] ERROR: missing proto file: ${PROTO_FILE}" >&2
  exit 1
fi

protoc_bin=""
grpc_plugin_bin=""
extra_ld_path=""

# Prefer repo-local bootstrap over system-global tool assumptions.
if [[ ! -x "${LOCAL_PROTOC}" || ! -x "${LOCAL_GRPC_PLUGIN}" ]]; then
  if "${ROOT_DIR}/scripts/ci/bootstrap_proto_tools.sh" >/dev/null 2>&1; then
    :
  fi
fi

if [[ -x "${LOCAL_PROTOC}" && -x "${LOCAL_GRPC_PLUGIN}" ]]; then
  protoc_bin="${LOCAL_PROTOC}"
  grpc_plugin_bin="${LOCAL_GRPC_PLUGIN}"
  extra_ld_path="${LOCAL_TOOLS_SYSROOT}/usr/lib/x86_64-linux-gnu:${LOCAL_TOOLS_SYSROOT}/usr/lib/$(uname -m)-linux-gnu:${LOCAL_TOOLS_SYSROOT}/usr/lib"
elif command -v protoc >/dev/null 2>&1 && command -v grpc_cpp_plugin >/dev/null 2>&1; then
  protoc_bin="$(command -v protoc)"
  grpc_plugin_bin="$(command -v grpc_cpp_plugin)"
fi

if [[ -n "${protoc_bin}" && -n "${grpc_plugin_bin}" ]]; then
  mkdir -p "${GEN_DIR}"

  if [[ -n "${extra_ld_path}" ]]; then
    env LD_LIBRARY_PATH="${extra_ld_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
      "${protoc_bin}" \
      --proto_path="${ROOT_DIR}/proto" \
      --cpp_out="${GEN_DIR}" \
      --grpc_out="${GEN_DIR}" \
      --plugin=protoc-gen-grpc="${grpc_plugin_bin}" \
      "${PROTO_FILE}"
  else
    "${protoc_bin}" \
      --proto_path="${ROOT_DIR}/proto" \
      --cpp_out="${GEN_DIR}" \
      --grpc_out="${GEN_DIR}" \
      --plugin=protoc-gen-grpc="${grpc_plugin_bin}" \
      "${PROTO_FILE}"
  fi

  required_outputs=(
    "${GEN_DIR}/kvstore/v1/kv.pb.h"
    "${GEN_DIR}/kvstore/v1/kv.pb.cc"
    "${GEN_DIR}/kvstore/v1/kv.grpc.pb.h"
    "${GEN_DIR}/kvstore/v1/kv.grpc.pb.cc"
  )

  for file in "${required_outputs[@]}"; do
    if [[ ! -f "${file}" ]]; then
      echo "[proto-compile] ERROR: expected generated file missing: ${file}" >&2
      exit 1
    fi
  done

  echo "[proto-compile] OK: protoc + grpc_cpp_plugin compile completed"
  exit 0
fi

echo "[proto-compile] BLOCKED: unable to locate usable protoc and grpc_cpp_plugin" >&2
echo "[proto-compile] INFO: running deterministic fallback checks (non-compile)" >&2

if ! grep -Eq '^syntax\s*=\s*"proto3"\s*;' "${PROTO_FILE}"; then
  echo "[proto-compile] ERROR: fallback syntax check failed: proto3 declaration missing" >&2
  exit 1
fi

if ! grep -Eq '^service\s+KV\s*\{' "${PROTO_FILE}"; then
  echo "[proto-compile] ERROR: fallback service check failed: service KV missing" >&2
  exit 1
fi

echo "[proto-compile] FALLBACK_OK: contract checks passed but compile is blocked by missing tooling" >&2
exit 2
