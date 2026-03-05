#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS_DIR="${ROOT_DIR}/.tools/grpc"
PACKAGES_DIR="${TOOLS_DIR}/packages"
SYSROOT_DIR="${TOOLS_DIR}/sysroot"
USR_DIR="${SYSROOT_DIR}/usr"
LOCAL_INCLUDE_DIR="${USR_DIR}/include"

multiarch=""
if command -v dpkg-architecture >/dev/null 2>&1; then
  multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)"
fi

LOCAL_LIB_DIRS=("${USR_DIR}/lib")
if [[ -n "${multiarch}" ]]; then
  LOCAL_LIB_DIRS=("${USR_DIR}/lib/${multiarch}" "${USR_DIR}/lib" "${USR_DIR}/lib/$(uname -m)-linux-gnu")
else
  LOCAL_LIB_DIRS=("${USR_DIR}/lib/$(uname -m)-linux-gnu" "${USR_DIR}/lib")
fi

have_headers=false
if [[ -f "${LOCAL_INCLUDE_DIR}/grpcpp/grpcpp.h" && -f "${LOCAL_INCLUDE_DIR}/google/protobuf/message.h" ]]; then
  have_headers=true
fi

have_libs=false
for d in "${LOCAL_LIB_DIRS[@]}"; do
  if [[ -f "${d}/libgrpc++.so" && -f "${d}/libgrpc.so" && -f "${d}/libprotobuf.so" ]]; then
    have_libs=true
    break
  fi
done

if [[ "${have_headers}" == true && "${have_libs}" == true ]]; then
  echo "[grpc-runtime] OK: repo-local gRPC/protobuf headers+libs already available under ${USR_DIR}"
  exit 0
fi

if ! command -v apt-get >/dev/null 2>&1; then
  echo "[grpc-runtime] ERROR: apt-get is required for repo-local bootstrap but not found" >&2
  exit 1
fi

if ! command -v dpkg-deb >/dev/null 2>&1; then
  echo "[grpc-runtime] ERROR: dpkg-deb is required for repo-local bootstrap but not found" >&2
  exit 1
fi

mkdir -p "${PACKAGES_DIR}" "${SYSROOT_DIR}"

# Keep the list explicit for determinism (similar to bootstrap_proto_tools.sh).
# We include both -dev and runtime packages so extracted sysroot can compile+link.
packages=(
  libgrpc++-dev
  libgrpc++1
  libgrpc-dev
  libgrpc6
  libprotobuf-dev
  libprotobuf17
  libc-ares-dev
  libc-ares2
  libre2-dev
  libre2-5
  zlib1g-dev
  zlib1g
  libssl-dev
)

# Distro variance: Ubuntu 22.04+ uses libssl3, older distros may use libssl1.1.
if command -v apt-cache >/dev/null 2>&1; then
  if apt-cache show libssl3 >/dev/null 2>&1; then
    packages+=(libssl3)
  elif apt-cache show libssl1.1 >/dev/null 2>&1; then
    packages+=(libssl1.1)
  fi
fi

(
  cd "${PACKAGES_DIR}"
  apt-get download "${packages[@]}"
)

for deb in "${PACKAGES_DIR}"/*.deb; do
  dpkg-deb -x "${deb}" "${SYSROOT_DIR}"
done

if [[ ! -f "${LOCAL_INCLUDE_DIR}/grpcpp/grpcpp.h" || ! -f "${LOCAL_INCLUDE_DIR}/google/protobuf/message.h" ]]; then
  echo "[grpc-runtime] ERROR: bootstrap completed but required headers are missing under ${LOCAL_INCLUDE_DIR}" >&2
  exit 1
fi

found=false
for d in "${LOCAL_LIB_DIRS[@]}"; do
  if [[ -f "${d}/libgrpc++.so" && -f "${d}/libgrpc.so" && -f "${d}/libprotobuf.so" ]]; then
    found=true
    break
  fi
done
if [[ "${found}" != true ]]; then
  echo "[grpc-runtime] ERROR: bootstrap completed but required libraries are missing under ${USR_DIR}/lib" >&2
  exit 1
fi

echo "[grpc-runtime] OK: repo-local gRPC/protobuf runtime bootstrapped at ${USR_DIR}"
