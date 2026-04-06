#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS_DIR="${ROOT_DIR}/.tools/proto"
PACKAGES_DIR="${TOOLS_DIR}/packages"
SYSROOT_DIR="${TOOLS_DIR}/sysroot"
LOCAL_BIN_DIR="${SYSROOT_DIR}/usr/bin"

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
    echo "[proto-tools] ERROR: unable to resolve ${label} package from apt metadata" >&2
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

declare -a packages=()
declare -A KVSTORE_SEEN_PACKAGES=()

append_package "protobuf-compiler"
append_package "protobuf-compiler-grpc"
append_package "$(resolve_package \
  "protobuf runtime" \
  "libprotobuf" \
  '^libprotobuf[0-9][0-9.t]*$' \
  libprotobuf17 libprotobuf23 libprotobuf25 libprotobuf32 libprotobuf32t64)"
append_package "$(resolve_package \
  "protoc runtime" \
  "libprotoc" \
  '^libprotoc[0-9][0-9.t]*$' \
  libprotoc17 libprotoc23 libprotoc25 libprotoc32 libprotoc32t64)"

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
