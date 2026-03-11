#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
COMMIT_RANGE=""
FAILURES=0

usage() {
  printf 'Usage: %s --range <git-range>\n' "${0##*/}" >&2
}

fail() {
  printf '[commit-messages] ERROR: %s\n' "$1" >&2
  exit 1
}

resolve_range() {
  local range="$1"
  local left
  local right

  if git -C "${ROOT_DIR}" rev-list "${range}" >/dev/null 2>&1; then
    printf '%s\n' "${range}"
    return 0
  fi

  case "${range}" in
    *..*)
      left="${range%%..*}"
      right="${range#*..}"
      ;;
    *)
      fail "invalid git revision range: ${range}"
      ;;
  esac

  case "${left}" in
    *~*)
      if ! git -C "${ROOT_DIR}" rev-parse --verify --quiet "${left}^{commit}" >/dev/null \
        && git -C "${ROOT_DIR}" rev-parse --verify --quiet "${right}^{commit}" >/dev/null; then
        printf '%s\n' "${right}"
        return 0
      fi
      ;;
  esac

  fail "invalid git revision range: ${range}"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --range)
      if [ "$#" -lt 2 ]; then
        fail 'missing value for --range'
      fi
      COMMIT_RANGE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      fail "unknown argument: $1"
      ;;
  esac
done

if [ -z "${COMMIT_RANGE}" ]; then
  usage
  fail 'missing required --range <git-range> argument'
fi

RESOLVED_RANGE="$(resolve_range "${COMMIT_RANGE}")"
HEADER_REGEX='^(feat|fix|docs|chore|refactor|perf|test|build|ci|style|revert)(\([a-z0-9][a-z0-9._/-]*\))?(!)?: .+$'
COMMITS="$(git -C "${ROOT_DIR}" rev-list --reverse "${RESOLVED_RANGE}")"

for sha in ${COMMITS}; do
  subject="$(git -C "${ROOT_DIR}" log -1 --format=%s "${sha}")"

  if ! printf '%s\n' "${subject}" | grep -Eq "${HEADER_REGEX}"; then
    printf '[commit-messages] ERROR: %s has invalid subject: %s\n' "${sha}" "${subject}" >&2
    FAILURES=1
  fi
done

if [ "${FAILURES}" -ne 0 ]; then
  exit 1
fi

COMMIT_COUNT="$(git -C "${ROOT_DIR}" rev-list --count "${RESOLVED_RANGE}")"
printf '[commit-messages] OK: %s commit subject(s) in %s match %s\n' "${COMMIT_COUNT}" "${COMMIT_RANGE}" "${HEADER_REGEX}"
