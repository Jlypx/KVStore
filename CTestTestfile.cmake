# Compatibility shim for CTest versions that ignore --test-dir.
# CTest 3.16 in this environment executes tests from source root.
add_test(kvstore_smoke_test "build/tests/kvstore_smoke_test")
add_test(kvstore_wal_test "build/tests/kvstore_wal_test")
add_test(kvstore_recovery_test "build/tests/kvstore_recovery_test")
add_test(kvstore_idempotency_test "build/tests/kvstore_idempotency_test")
add_test(kvstore_sstable_test "build/tests/kvstore_sstable_test")
add_test(kvstore_cache_test "build/tests/kvstore_cache_test")
add_test(kvstore_compaction_test "build/tests/kvstore_compaction_test")
add_test(kvstore_raft_election_test "build/tests/kvstore_raft_election_test")
add_test(kvstore_raft_replication_test "build/tests/kvstore_raft_replication_test")
add_test(kvstore_raft_quorum_test "build/tests/kvstore_raft_quorum_test")
add_test(kvstore_raft_failover_test "build/tests/kvstore_raft_failover_test")
add_test(kvstore_grpc_integration_test "build/tests/grpc/kvstore_grpc_integration_test")
add_test(kvstore_grpc_idempotency_test "build/tests/grpc/kvstore_grpc_idempotency_test")
add_test(kvstore_api_status_test "build/tests/grpc/kvstore_api_status_test")
add_test(
  kvstore_tls_profile_toggle_test
  "build/tests/grpc/kvstore_tls_profile_toggle_test"
  "tests/assets/tls/server.crt"
  "tests/assets/tls/server.key"
)
add_test(kvstore_integration_failover_test "build/tests/integration/kvstore_integration_failover_test")
add_test(kvstore_chaos_gate_test "build/tests/integration/kvstore_chaos_gate_test" "failover")
add_test(kvstore_integrity_gate_test "python3" "scripts/integrity/run_corruption_suite.py" "--repo-root" "." "--build-dir" "build")
add_test(kvstore_bench_gate_test "build/tests/integration/kvstore_bench_gate_test")

set_tests_properties(
  kvstore_grpc_integration_test
  kvstore_grpc_idempotency_test
  kvstore_api_status_test
  kvstore_tls_profile_toggle_test
  PROPERTIES
  ENVIRONMENT "LD_LIBRARY_PATH=.tools/grpc/sysroot/usr/lib/x86_64-linux-gnu:.tools/grpc/sysroot/usr/lib:$ENV{LD_LIBRARY_PATH}"
)
