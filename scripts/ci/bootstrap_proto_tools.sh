#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS_DIR="${ROOT_DIR}/.tools/proto"
PACKAGES_DIR="${TOOLS_DIR}/packages"
SYSROOT_DIR="${TOOLS_DIR}/sysroot"
LOCAL_BIN_DIR="${SYSROOT_DIR}/usr/bin"

if [[ -x "${LOCAL_BIN_DIR}/protoc" && -x "${LOCAL_BIN_DIR}/grpc_cpp_plugin" ]]; then
  echo "[proto-tools] OK: repo-local tools already available at ${LOCAL_BIN_DIR}"
  exit 0
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "[proto-tools] ERROR: apt-get is required for repo-local bootstrap but not found" >&2
  exit 1
fi

if ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "[proto-tools] ERROR: dpkg-deb is required for repo-local bootstrap but not found" >&2
  exit 1
fi

mkdir -p "${PACKAGES_DIR}" "${SYSROOT_DIR}"

packages=(
  protobuf-compiler
  protobuf-compiler-grpc
  libprotobuf17
  libprotoc17
)

(
  cd "${PACKAGES_DIR}"
  apt-get download "${packages[@]}"
)

for deb in "${PACKAGES_DIR}"/*.deb; do
  dpkg-deb -x "${deb}" "${SYSROOT_DIR}"
done

if [[ ! -x "${LOCAL_BIN_DIR}/protoc" || ! -x "${LOCAL_BIN_DIR}/grpc_cpp_plugin" ]]; then
  echo "[proto-tools] ERROR: bootstrap completed but required binaries are missing under ${LOCAL_BIN_DIR}" >&2
  exit 1
fi

echo "[proto-tools] OK: repo-local protoc/grpc_cpp_plugin bootstrapped at ${LOCAL_BIN_DIR}"
