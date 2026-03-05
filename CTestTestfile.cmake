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
