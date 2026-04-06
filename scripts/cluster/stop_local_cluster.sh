#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUN_DIR="${ROOT_DIR}/.cluster-run"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run-dir)
      RUN_DIR="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ ! -d "${RUN_DIR}/pids" ]]; then
  exit 0
fi

for pidfile in "${RUN_DIR}"/pids/*.pid; do
  [[ -e "${pidfile}" ]] || continue
  pid="$(cat "${pidfile}")"
  kill "${pid}" 2>/dev/null || true
done

sleep 1

for pidfile in "${RUN_DIR}"/pids/*.pid; do
  [[ -e "${pidfile}" ]] || continue
  pid="$(cat "${pidfile}")"
  kill -9 "${pid}" 2>/dev/null || true
done
