# KVStore v1 Testing and Validation

## Purpose

This document records how the current repository validates v1 behavior across correctness, failover, integrity, performance, and runtime transport handling.
It is limited to checks that exist in the tree today and to evidence captured in Tasks 5 through 7.
It does not claim CI jobs, distributed infra, Jepsen coverage, dynamic membership tests, or long-running soak validation that are not present in the repository.

## Test layers / strategy

The current validation stack now has three layers.

| Layer | Scope | Main artifacts | What it proves |
|---|---|---|---|
| Deterministic unit and integration style | In-process binaries against `TestCluster`, `KvRaftService`, storage readers, and gRPC adapters | `tests/raft_failover_test.cpp`, `tests/integration/chaos_gate_test.cpp`, `tests/integration/integrity_gate_test.cpp`, `tests/integration/bench_gate_test.cpp`, `tests/grpc/api_status_test.cpp`, `tests/grpc/grpc_integration_test.cpp`, `tests/grpc/grpc_idempotency_test.cpp`, `tests/grpc/tls_profile_toggle_test.cpp` | Logical correctness, failure handling, checksum enforcement, status mapping, and transport profile semantics in a repeatable local process |
| WSL and runtime smoke style | Real `kvd` process plus external client smoke wrappers and evidence capture scripts | `scripts/chaos/partition_heal_check.py`, `scripts/chaos/kill_leader_and_assert.py`, `scripts/chaos/assert_restart_rto.py`, `scripts/integrity/run_corruption_suite.py`, `scripts/bench/run_baseline.sh`, `scripts/bench/assert_slo.py` | That the built binaries run end to end in the current environment, emit machine-readable evidence, and preserve the same behavior when exercised through the shipped runtime entry points |
| Same-host multi-process cluster | Five real `kvd --mode=cluster-node` processes on loopback with peer gRPC | `tests/integration/multi_process_cluster_test.cpp`, `scripts/cluster/start_local_cluster.sh`, `scripts/cluster/stop_local_cluster.sh` | That leader election, redirection, replication, and failover also work across real processes rather than only in the embedded transport |

The important split is that most correctness and storage checks are deterministic and in-process, while the Task 7 transport and profile checks are runtime smoke checks that explicitly run through WSL-facing paths and external client connections.

## Deterministic Raft tests

`tests/raft_failover_test.cpp` is the smallest direct Raft correctness check.
It uses `kvstore::raft::TestCluster` only, without the gRPC layer or storage wrappers, to verify these invariants:

- a five-node cluster elects a leader
- a committed log entry survives leader failure
- a different leader is elected after the original leader is taken down
- the new leader can commit a new entry with quorum still available
- the recovered former leader catches up and preserves both committed commands

This test is deterministic in structure because message delivery and ticking stay inside the test transport.
It is the core Task 5 proof that failover preserves committed state before higher-level service and runtime coverage are added.

`tests/integration/chaos_gate_test.cpp` extends the same Raft and service behavior into deterministic integration modes:

- `failover`, leader replacement and post-failover write success within `max_failover_ms=5000`
- `restart_rto`, service restart plus value recovery within `max_rto_ms=60000`
- `partition_heal`, write rejection during quorum loss and recovery after healing

These modes are later wrapped by Task 7 scripts to turn the same checks into preserved JSON evidence.

## gRPC and service integration tests

The client-facing API and service semantics are covered by direct gRPC tests:

- `tests/grpc/api_status_test.cpp` verifies service error to gRPC status translation, including `INVALID_ARGUMENT`, `FAILED_PRECONDITION` with `leader_hint=<n>`, `UNAVAILABLE`, and `DEADLINE_EXCEEDED`
- `tests/grpc/grpc_integration_test.cpp` verifies end-to-end `Put`, `Get`, and `Delete` behavior, then forces quorum loss and confirms both writes and linearizable reads fail with `UNAVAILABLE`
- `tests/grpc/grpc_idempotency_test.cpp` verifies `Put` idempotency on repeated `request_id` reuse and rejects conflicting retries as `INVALID_ARGUMENT`
- `tests/grpc/tls_profile_toggle_test.cpp` verifies that both `dev` and `secure` listener profiles preserve the same `Put` and `Get` semantics, and reports `"semantic_drift":false` in in-process mode

Together these tests validate the shipped v1 surface, not just internal state machine behavior.
They prove that correctness survives translation through the protobuf and gRPC boundary, including the current status-code contract and the current request-idempotency behavior.

## Storage and integrity validation

Storage correctness is validated through targeted corruption and replay checks, not only through happy-path reads and writes.

`tests/integration/integrity_gate_test.cpp` exposes four explicit modes:

- `make_wal`, create a real WAL by writing through `KvEngine`
- `replay_wal`, replay WAL records and surface integrity failures
- `make_sst`, flush an SSTable from real engine state
- `read_sst`, open and read SST data while surfacing integrity failures

`scripts/integrity/run_corruption_suite.py` composes those modes with the repository corruption helpers and writes machine-readable evidence.
It uses:

- `scripts/integrity/corrupt_wal_byte.py`
- `scripts/integrity/corrupt_sst_block.py`
- `tests/integration/integrity_gate_test.cpp`, resolved at runtime as `kvstore_integrity_gate_test`

The authoritative Task 7 integrity artifact is `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`.
That evidence records `pass=true` and shows both corruption paths failing closed with `CHECKSUM_MISMATCH`:

- WAL replay returns `"integrity_code":"CHECKSUM_MISMATCH"`
- SST read returns `"integrity_code":"CHECKSUM_MISMATCH"`

This is the repository's proof that corrupted persisted state is detected on replay and read, not silently accepted.

## Chaos and recovery validation

Chaos and recovery coverage is built around `tests/integration/chaos_gate_test.cpp` and three thin wrappers under `scripts/chaos/`.

- `scripts/chaos/kill_leader_and_assert.py` runs `kvstore_chaos_gate_test failover` and writes `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`
- `scripts/chaos/assert_restart_rto.py` runs `kvstore_chaos_gate_test restart_rto` and writes `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`
- `scripts/chaos/partition_heal_check.py` runs `kvstore_chaos_gate_test partition_heal`, then combines that result with runtime TLS smoke checks and writes `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`

Task 7 preserved concrete pass results for these recovery paths:

- `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`, `pass=true`, `leader_before=4`, `leader_after=3`, `failover_ms=10`, `max_failover_ms=5000`
- `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`, `pass=true`, `restart_rto_ms=1`, `max_rto_ms=60000`
- `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`, `partition_heal.pass=true`, `partition_write_rejected=true`

These checks are still local-process validation because the Raft cluster is embedded, but they exercise the failure semantics that matter for v1, leader replacement, restart recovery, quorum loss rejection, and successful recovery after healing.

## Performance and SLO validation

Performance validation is intentionally narrow and evidence-backed.
It is not a broad load test suite.

`tests/integration/bench_gate_test.cpp` measures 300 durable writes and 300 linearizable reads, computes p99 latency for each, then verifies a stronger correctness gate by checking that an acknowledged write survives a leader crash.
The companion scripts are:

- `scripts/bench/run_baseline.sh`, run the benchmark binary and capture raw JSON output
- `scripts/bench/assert_slo.py`, assert repository thresholds of `max_write_p99_ms=20.0`, `max_read_p99_ms=10.0`, and `no_acknowledged_write_loss=true`

Task 7 produced two relevant benchmark artifacts:

- `.sisyphus/evidence/task7-checks/bench/task-7-bench.json`, an earlier failed gate with `{"pass":false,"reason":"acknowledged_write_lost"}`
- `.sisyphus/evidence/task7-post-fix/task-7-bench.json`, the authoritative post-fix result with `pass=true`, `samples=300`, `p99_durable_write_ms=1.416`, `p99_read_ms=0.002`, and `no_acknowledged_write_loss=true`

The second file is the one that should be cited for the current v1 state because it captures the final post-fix benchmark outcome from Task 7.

## Runtime TLS/profile validation

Runtime transport validation is intentionally separated from the deterministic tests.
This is the part of the repository that proves the built `kvd` binary starts with both supported listener profiles and can be exercised by an external client.

`tests/grpc/tls_profile_toggle_test.cpp` supports two execution modes:

- in-process mode, compare `dev` and `secure` semantics directly
- external mode, connect to a live target with `--mode=external --target=HOST:PORT --tls_profile=dev|secure`

`scripts/chaos/partition_heal_check.py` is the Task 7 runtime wrapper for that behavior.
It launches:

- `build/src/kvd` with `--tls_profile=dev` or `--tls_profile=secure`
- `build/tests/grpc/kvstore_tls_profile_toggle_test` in external mode against the live `kvd` listener

The preserved runtime artifact is `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`.
It records successful external smoke runs for both profiles:

- `dev`, `target=127.0.0.1:44657`, `smoke.pass=true`
- `secure`, `target=127.0.0.1:41985`, `smoke.pass=true`

Supporting per-run outputs are also preserved in:

- `.sisyphus/evidence/task7-final/task-7-partition-heal.log`
- `.sisyphus/evidence/task7-final/task-7-tls-toggle.log`
- `.sisyphus/evidence/task7-suite/task-7-partition-heal.log`
- `.sisyphus/evidence/task7-suite/task-7-tls-toggle.log`

These are runtime smoke checks, not a separate networked Raft transport test.
They validate the client-facing gRPC listener, TLS credential loading, and semantic parity across `dev` and `secure` profiles in the current WSL-backed execution path.

## Evidence and reproducibility

The repository keeps the main Task 7 outputs under `.sisyphus/evidence/`.
For the current v1 state, the most important files are:

- `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`
- `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`
- `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`
- `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
- `.sisyphus/evidence/task7-post-fix/task-7-bench.json`

Supporting logs and earlier suite outputs remain useful for traceability:

- `.sisyphus/evidence/task7-final/task-7-partition-heal.log`
- `.sisyphus/evidence/task7-final/task-7-tls-toggle.log`
- `.sisyphus/evidence/task7-suite/chaos_kill_leader_and_assert.json`
- `.sisyphus/evidence/task7-suite/chaos_assert_restart_rto.json`
- `.sisyphus/evidence/task7-suite/integrity_corruption_suite.json`
- `.sisyphus/evidence/task7-suite/chaos_partition_heal_and_tls.json`

Reproduction uses the same repository entry points that produced the saved evidence:

- build the tree so `kvstore_chaos_gate_test`, `kvstore_integrity_gate_test`, `kvstore_bench_gate_test`, `kvstore_tls_profile_toggle_test`, and `kvd` exist under `build/`
- run `scripts/chaos/kill_leader_and_assert.py` for failover evidence
- run `scripts/chaos/assert_restart_rto.py` for restart recovery evidence
- run `scripts/integrity/run_corruption_suite.py` for checksum detection evidence
- run `scripts/bench/run_baseline.sh` and `scripts/bench/assert_slo.py` for benchmark and SLO evidence
- run `scripts/chaos/partition_heal_check.py` for partition-heal plus runtime TLS/profile smoke evidence

Because the runtime transport checks launch `kvd` and connect with an external test client, those checks are environment-sensitive and are expected to run through the same WSL-style path used in Task 7.

## Known limitations / deferred validation

The current validation story is strong for implemented v1 behavior, but it has clear boundaries.

- There is no Jepsen-style distributed fault campaign.
- There is no dynamic membership validation because dynamic membership is out of scope for v1.
- There is no multi-process peer transport validation because Raft peers are embedded through `TestCluster` and `TestTransport`.
- There is no long-duration soak or multi-node disk-fault matrix beyond the targeted corruption suite.
- The benchmark gate is a local-process acceptance check, not a production capacity model.
- TLS coverage is limited to the client-facing listener profiles `dev` and `secure`. It does not imply a separate TLS-protected inter-node transport.

Those omissions are intentional for the current v1 boundary and should be treated as deferred work, not as hidden guarantees. The new same-host multi-process cluster coverage closes the previous gap where all Raft validation depended on an embedded transport, but it is still not the same as cross-machine distributed validation.
