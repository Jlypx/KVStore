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

have_apt_package() {
  apt-cache show "$1" >/dev/null 2>&1
}

resolve_exact_package() {
  local candidate
  for candidate in "$@"; do
    if [[ -n "${candidate}" ]] && have_apt_package "${candidate}"; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

resolve_prefix_package() {
  local prefix="$1"
  local pattern="$2"
  local selected=""
  local candidate
  while IFS= read -r candidate; do
    if [[ "${candidate}" =~ ${pattern} ]] && have_apt_package "${candidate}"; then
      selected="${candidate}"
    fi
  done < <(apt-cache pkgnames | awk -v pfx="${prefix}" 'index($0, pfx) == 1 { print $0 }' | sort -V)

  if [[ -n "${selected}" ]]; then
    echo "${selected}"
    return 0
  fi
  return 1
}

resolve_package() {
  local label="$1"
  local prefix="$2"
  local pattern="$3"
  shift 3

  local resolved=""
  resolved="$(resolve_exact_package "$@" || true)"
  if [[ -z "${resolved}" ]]; then
    resolved="$(resolve_prefix_package "${prefix}" "${pattern}" || true)"
  fi
  if [[ -z "${resolved}" ]]; then
    echo "[grpc-runtime] ERROR: unable to resolve ${label} package from apt metadata" >&2
    return 1
  fi
  echo "${resolved}"
}

append_package() {
  local pkg="$1"
  if [[ -z "${pkg}" ]]; then
    return 0
  fi
  if [[ -n "${KVSTORE_SEEN_PACKAGES[${pkg}]:-}" ]]; then
    return 0
  fi
  packages+=("${pkg}")
  KVSTORE_SEEN_PACKAGES["${pkg}"]=1
}

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

declare -a packages=()
declare -A KVSTORE_SEEN_PACKAGES=()

# Keep the dev package list explicit, but resolve runtime soname packages
# dynamically because ubuntu-latest periodically renames them (for example t64
# transitions and newer protobuf/grpc ABI bumps).
append_package "libgrpc++-dev"
append_package "$(resolve_package \
  "gRPC++ runtime" \
  "libgrpc++" \
  '^libgrpc\+\+[0-9][0-9.t]*$' \
  libgrpc++1 libgrpc++1.51 libgrpc++1.51t64)"
append_package "libgrpc-dev"
append_package "$(resolve_package \
  "gRPC runtime" \
  "libgrpc" \
  '^libgrpc[0-9][0-9.t]*$' \
  libgrpc6 libgrpc29 libgrpc29t64)"
append_package "libprotobuf-dev"
append_package "$(resolve_package \
  "protobuf runtime" \
  "libprotobuf" \
  '^libprotobuf[0-9][0-9.t]*$' \
  libprotobuf17 libprotobuf23 libprotobuf25 libprotobuf32 libprotobuf32t64)"
append_package "libc-ares-dev"
append_package "$(resolve_package \
  "c-ares runtime" \
  "libc-ares" \
  '^libc-ares[0-9][0-9.t-]*$' \
  libc-ares2 libc-ares2t64)"
append_package "libre2-dev"
append_package "$(resolve_package \
  "re2 runtime" \
  "libre2-" \
  '^libre2-[0-9][0-9.t-]*$' \
  libre2-5 libre2-9 libre2-10 libre2-11)"
append_package "zlib1g-dev"
append_package "zlib1g"
append_package "libssl-dev"
append_package "$(resolve_package \
  "OpenSSL runtime" \
  "libssl" \
  '^libssl[0-9.]+(t64)?$' \
  libssl3 libssl3t64 libssl1.1)"

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
