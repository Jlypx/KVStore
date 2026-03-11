#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
TAG='[release-readiness]'
PYTHON_BIN=''

log() {
  printf '%s %s\n' "${TAG}" "$1"
}

fail() {
  printf '%s ERROR: %s\n' "${TAG}" "$1" >&2
  exit 1
}

require_file() {
  local rel_path="$1"
  local abs_path="${ROOT_DIR}/${rel_path}"

  if [ ! -f "${abs_path}" ]; then
    fail "missing required file: ${rel_path}"
  fi
}

select_python() {
  if command -v python3 >/dev/null 2>&1 \
    && python3 -c 'import sys; raise SystemExit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; then
    PYTHON_BIN='python3'
    return 0
  fi

  if command -v python >/dev/null 2>&1 \
    && python -c 'import sys; raise SystemExit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; then
    PYTHON_BIN='python'
    return 0
  fi

  if command -v py >/dev/null 2>&1 \
    && py -3 -c 'import sys' >/dev/null 2>&1; then
    PYTHON_BIN='py'
    return 0
  fi

  fail 'no Python 3 interpreter found (tried python3, python, and py -3)'
}

run_python() {
  if [ -z "${PYTHON_BIN}" ]; then
    fail 'python interpreter not selected'
  fi

  if [ "${PYTHON_BIN}" = 'py' ]; then
    py -3 "$@"
    return 0
  fi

  "${PYTHON_BIN}" "$@"
}

require_contains() {
  local rel_path="$1"
  local needle="$2"
  local description="$3"
  local abs_path="${ROOT_DIR}/${rel_path}"

  require_file "${rel_path}"

  if ! run_python - "${abs_path}" "${needle}" <<'PY'; then
from pathlib import Path
import sys

path = Path(sys.argv[1])
needle = sys.argv[2].encode("utf-8")
data = path.read_bytes()
raise SystemExit(0 if needle in data else 1)
PY
    fail "${description}: missing marker '${needle}' in ${rel_path}"
  fi
}

require_json_true() {
  local rel_path="$1"
  local key="$2"
  local description="$3"
  local abs_path="${ROOT_DIR}/${rel_path}"

  require_file "${rel_path}"

  if ! run_python - "${abs_path}" "${key}" <<'PY'; then
from pathlib import Path
import json
import sys

path = Path(sys.argv[1])
key = sys.argv[2]
payload = json.loads(path.read_text(encoding="utf-8"))
raise SystemExit(0 if payload.get(key) is True else 1)
PY
    fail "${description}: expected JSON key '${key}' to be true in ${rel_path}"
  fi
}

main() {
  cd "${ROOT_DIR}"

  log 'selecting Python 3 interpreter'
  select_python
  if [ "${PYTHON_BIN}" = 'py' ]; then
    log 'using Python interpreter: py -3'
  else
    log "using Python interpreter: ${PYTHON_BIN}"
  fi

  log 'checking release policy markers in docs/release.md'
  require_file 'docs/release.md'
  require_contains 'docs/release.md' '## Release checklist and readiness gate' 'release policy'
  require_contains 'docs/release.md' '## Versioning policy for the v1 line' 'release policy'
  require_contains 'docs/release.md' '## Changelog policy and template guidance' 'release policy'
  require_contains 'docs/release.md' '## Git governance, authorship, and branch strategy' 'release policy'
  require_contains 'docs/release.md' 'jlypx' 'release policy'
  require_contains 'docs/release.md' 'Co-authored-by:' 'release policy'
  log 'policy markers OK'

  log 'running docs baseline gates'
  bash scripts/docs/check_required_docs.sh
  bash scripts/docs/check_command_blocks.sh
  log 'docs gates OK'

  log 'running study timeline gate for Task 9 upper bound'
  require_file 'study.md'
  run_python scripts/docs/check_study_timeline.py --file study.md --expected-tasks 9
  log 'study timeline gate OK'

  log 'running recent commit subject gate'
  bash scripts/git/check_commit_messages.sh --range HEAD~20..HEAD
  log 'commit message gate OK'

  log 'checking Task 7 and Task 8 evidence artifacts'
  require_json_true '.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json' 'pass' 'failover evidence'
  require_json_true '.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json' 'pass' 'restart evidence'
  require_json_true '.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json' 'pass' 'integrity evidence'
  require_contains '.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json' 'CHECKSUM_MISMATCH' 'integrity evidence'
  require_json_true '.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json' 'pass' 'partition-heal and TLS evidence'
  require_json_true '.sisyphus/evidence/task7-post-fix/task-7-bench.json' 'pass' 'benchmark evidence'
  require_json_true '.sisyphus/evidence/task7-post-fix/task-7-bench.json' 'no_acknowledged_write_loss' 'benchmark evidence'
  require_contains '.sisyphus/evidence/task-8-doc-gate.log' '[required-docs] OK: required Task 8 docs and core headings are present' 'Task 8 docs gate log'
  require_contains '.sisyphus/evidence/task-8-doc-gate.log' '[command-blocks] OK: docs include concrete build/test commands, Task 7 validation commands, and .sisyphus/evidence references' 'Task 8 docs gate log'
  require_contains '.sisyphus/evidence/task-8-study-check.log' '[study-timeline] OK:' 'Task 8 study log'
  require_contains '.sisyphus/evidence/task-8-study-check.log' 'upper bound: 9' 'Task 8 study log'
  log 'evidence markers OK'

  log 'OK: release readiness policy markers, docs gates, commit gate, and Task 7/8 evidence all passed'
}

main "$@"
