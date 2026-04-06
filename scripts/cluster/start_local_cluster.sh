#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RUN_DIR="${ROOT_DIR}/.cluster-run"
CONFIG_DIR="${ROOT_DIR}/deploy"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --run-dir)
      RUN_DIR="$2"
      shift 2
      ;;
    --config-dir)
      CONFIG_DIR="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

mkdir -p "${RUN_DIR}/logs" "${RUN_DIR}/pids"

for node_id in 1 2 3 4 5; do
  config="${CONFIG_DIR}/kvd_cluster_node_${node_id}.toml"
  log="${RUN_DIR}/logs/node${node_id}.log"
  pidfile="${RUN_DIR}/pids/node${node_id}.pid"
  if [[ ! -f "${config}" ]]; then
    echo "missing config: ${config}" >&2
    exit 1
  fi
  "${BUILD_DIR}/src/kvd" --mode=cluster-node --config="${config}" >"${log}" 2>&1 &
  echo $! >"${pidfile}"
done

sleep 1

for node_id in 1 2 3 4 5; do
  pidfile="${RUN_DIR}/pids/node${node_id}.pid"
  pid="$(cat "${pidfile}")"
  if ! kill -0 "${pid}" 2>/dev/null; then
    echo "node ${node_id} failed to stay alive" >&2
    exit 1
  fi
done

echo "cluster started with run dir ${RUN_DIR}"
