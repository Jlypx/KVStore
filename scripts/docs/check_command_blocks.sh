#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FAILURES=0
DOC_FILES="docs/operations.md docs/testing.md docs/release.md"

check_doc_file() {
  local rel_path="$1"
  local abs_path="${ROOT_DIR}/${rel_path}"

  if [[ ! -f "${abs_path}" ]]; then
    printf '[command-blocks] ERROR: missing docs input: %s\n' "${rel_path}" >&2
    FAILURES=1
  fi
}

doc_contains_any() {
  local needle
  local rel_path
  local abs_path

  for needle in "$@"; do
    for rel_path in ${DOC_FILES}; do
      abs_path="${ROOT_DIR}/${rel_path}"

      if [[ -f "${abs_path}" ]] && grep -Fq -- "${needle}" "${abs_path}"; then
        return 0
      fi
    done
  done

  return 1
}

check_any() {
  local label="$1"
  shift
  local expected="$*"

  if ! doc_contains_any "$@"; then
    printf '[command-blocks] ERROR: missing %s in docs/operations.md, docs/testing.md, or docs/release.md\n' "${label}" >&2
    printf '[command-blocks] ERROR: expected one of: %s\n' "${expected}" >&2
    FAILURES=1
  fi
}

for rel_path in ${DOC_FILES}; do
  check_doc_file "${rel_path}"
done

check_any \
  'configure/build command coverage' \
  'cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug' \
  'cmake --build build -j' \
  'scripts/ci/local_check.sh'

check_any \
  'test command coverage' \
  '(cd build && ctest --output-on-failure)' \
  'scripts/ci/local_check.sh'

check_any \
  'Task 7 chaos validation command coverage' \
  'scripts/chaos/kill_leader_and_assert.py' \
  'scripts/chaos/assert_restart_rto.py' \
  'scripts/chaos/partition_heal_check.py'

check_any \
  'Task 7 integrity validation command coverage' \
  'scripts/integrity/run_corruption_suite.py'

check_any \
  'Task 7 benchmark validation command coverage' \
  'scripts/bench/run_baseline.sh' \
  'scripts/bench/assert_slo.py'

check_any \
  'Task 7 chaos/recovery evidence references under .sisyphus/evidence/' \
  '.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json' \
  '.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json' \
  '.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json'

check_any \
  'Task 7 integrity evidence references under .sisyphus/evidence/' \
  '.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json'

check_any \
  'Task 7 benchmark evidence references under .sisyphus/evidence/' \
  '.sisyphus/evidence/task7-post-fix/task-7-bench.json'

if [[ "${FAILURES}" -ne 0 ]]; then
  exit 1
fi

printf '[command-blocks] OK: docs include concrete build/test commands, Task 7 validation commands, and .sisyphus/evidence references\n'
