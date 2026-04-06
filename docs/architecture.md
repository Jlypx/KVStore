# KVStore v1 Architecture

## Purpose

This document describes the current implemented v1 architecture.
It ties the code layout, frozen scope contract, ADR decisions, and Task 7 runtime evidence into one view.
It does not widen scope beyond `docs/architecture/scope.md`.

## System Overview

- External API: unary gRPC service `kvstore.v1.KV` with `Put`, `Get`, and `Delete` in `proto/kvstore/v1/kv.proto`.
- Server binary: `src/bin/kvd.cpp` starts one client-facing gRPC listener and registers `kvstore::api::GrpcKvService`.
- Service core: `src/service/kv_raft_service.cpp` owns request validation, idempotency handling, leader checks, and orchestration.
- Consensus core: `src/raft/raft_node.cpp` implements leader election, log replication, quorum contact checks, and commit advancement.
- Storage core: each Raft node owns a `kvstore::engine::KvEngine` backed by a WAL, memtable, SST files, compaction, and block cache.
- Integrity core: `src/integrity/crc32c.cpp`, `src/engine/wal.cpp`, and `src/engine/sstable.cpp` verify checksums on replay and read paths.

The repository now supports two runtime forms:

- `embedded` mode, which keeps the original single `kvd` process with an embedded static five-node cluster through `kvstore::raft::TestCluster` in `src/raft/test_transport.cpp`
- `cluster-node` mode, where one `kvd` process represents one real node and communicates with peers over a dedicated internal gRPC service defined in `proto/kvstore/v1/raft.proto`

In `cluster-node` mode, each node persists only its own engine and Raft state under its configured `data_dir`.

## Node and Service Architecture

### Client-facing service

- `kvd` exposes one gRPC endpoint.
- `GrpcKvService` converts protobuf requests into service calls and maps service errors to gRPC status codes.
- Supported TLS profiles are `dev` and `secure`, implemented in `src/bin/kvd.cpp` and exercised by `tests/grpc/tls_profile_toggle_test.cpp`.

### Embedded cluster

- `KvRaftService` constructs a static five-node cluster by default.
- `TestCluster` and `TestTransport` provide deterministic in-process peer messaging and node up/down control.
- Each logical node has its own `KvEngine` instance and storage directory.
- A background ticker drives Raft time, elections, heartbeat emission, message delivery, and commit callbacks.

### Cluster-node runtime

- `kvd --mode=cluster-node --config=...` starts one real node process.
- Each node exposes one client-facing gRPC listener and one peer-only gRPC listener.
- Peer traffic uses `kvstore.v1.RaftPeer` from `proto/kvstore/v1/raft.proto`.
- `NetworkTransport` sends Raft RPCs across processes, while `InProcessTransport` remains available for embedded tests.

### Read and write semantics

- `Put` and `Delete` are accepted only through the current leader and only while quorum contact is present.
- `Get` is served from the leader only, and it is rejected when quorum contact is lost.
- A write is acknowledged only after the command is committed and applied through the state machine callback.

## Module Boundaries

| Boundary | Current implementation | Evidence |
|---|---|---|
| `api` | Protobuf surface and gRPC status translation only | `proto/kvstore/v1/kv.proto`, `src/api/grpc_kv_service.cpp` |
| `service` | Request validation, idempotency, leader routing, orchestration | `src/service/kv_raft_service.cpp` |
| `raft` | Election, replication, quorum checks, durable Raft state, in-process and network transport, snapshot install and log truncation | `src/raft/raft_node.cpp`, `src/raft/test_transport.cpp`, `src/raft/network_transport.cpp`, `src/raft/raft_storage.cpp` |
| `engine` | WAL, memtable, SST read/write, compaction, per-node state machine storage | `src/engine/kv_engine.cpp`, `src/engine/wal.cpp`, `src/engine/sstable.cpp`, `src/engine/compaction.cpp` |
| `cache` | SST block cache for repeated reads | `src/cache/block_cache.cpp` |
| `integrity` | CRC32C checksum generation and verification, integrity error propagation | `src/integrity/crc32c.cpp`, `src/engine/wal.cpp`, `src/engine/sstable.cpp` |
| `observability` | Reserved by the scope contract, not implemented as a standalone runtime module in the current tree | `docs/architecture/scope.md` |

These boundaries match the v1 scope contract in `docs/architecture/scope.md`. The current code adds an explicit `raft` layer between `service` and `engine`, which is consistent with the implemented request path and does not change the frozen API boundary.

## Request Lifecycle

### `Put`

1. Client sends `Put` to `kvstore.v1.KV`.
2. `GrpcKvService::Put` forwards the request and deadline to `KvRaftService::Put`.
3. `KvRaftService` validates key, value, and `request_id`, then checks the completed and in-flight idempotency maps.
4. The leader encodes the mutation into a versioned binary command and calls `RaftNode::Propose`.
5. The leader replicates the log entry to followers and advances commit only after majority replication in the current term.
6. Commit callbacks invoke `KvEngine::Put` on each node.
7. `KvEngine::Put` appends a checksummed WAL record, applies the memtable mutation, and records idempotent request state.
8. The original RPC returns success only after the committed result is available to the waiting request future.

### `Get`

1. Client sends `Get` to `kvstore.v1.KV`.
2. `GrpcKvService::Get` calls `KvRaftService::Get`.
3. `KvRaftService` requires a current leader and `HasQuorumContact()`.
4. The leader-owned `KvEngine` serves the read from memtable first, then from SSTables newest to oldest.
5. SST access verifies block and footer checksums before data is trusted.

### `Delete`

1. Client sends `Delete` to `kvstore.v1.KV`.
2. `GrpcKvService::Delete` forwards the client-supplied `request_id` and deadline to `KvRaftService::Delete`.
3. The remaining flow matches `Put`: service validation, leader proposal, Raft commit, WAL append, memtable apply, and response after commit.

## Storage and Integrity Components Summary

### WAL

- Implemented in `src/engine/wal.cpp`.
- Record header stores magic, version, operation, key size, value size, request id size, and checksum.
- Replay rejects invalid magic, unsupported version, invalid sizes, truncated records, and checksum mismatch.
- This is the first durability step for committed mutations.

### Memtable and idempotency

- `KvEngine` applies mutations to an in-memory memtable after WAL append.
- The engine and service both preserve idempotent behavior through `request_id` tracking.
- Duplicate replay or duplicate client submission does not create a second applied mutation.

### SSTables and compaction

- Implemented in `src/engine/sstable.cpp` and summarized in `docs/storage-format.md`.
- SSTables are immutable, sorted, block-framed files with an index block and checksummed footer.
- `KvEngine::Flush()` writes a new SST from memtable contents.
- `KvEngine::Compact()` merges existing SSTs into one new SST and replaces old files.

### Snapshots

- Raft snapshot metadata and log truncation are implemented in `src/raft/raft_node.cpp` and `src/raft/raft_storage.cpp`.
- State-machine snapshot export and install are implemented in `src/engine/kv_engine.cpp`.
- Peer snapshot transfer uses the internal `kvstore.v1.RaftPeer` service in `proto/kvstore/v1/raft.proto`.
- A lagging follower can now catch up by installing a snapshot when it falls behind the leader's truncated log prefix.

### Block cache

- Implemented in `src/cache/block_cache.cpp`.
- The cache stores SST block payloads by file id, offset, and frame size.
- It is a read acceleration layer only. It is not part of the durability contract.

### Integrity handling

- CRC32C is implemented in `src/integrity/crc32c.cpp`.
- WAL replay verifies record checksums before applying mutations.
- SST open and read paths verify footer, index, and data-block checksums before returning data.
- Integrity failures surface as explicit errors. The system does not silently trust corrupted on-disk state.

## Operational Topology and Deployment Assumptions for v1

- Membership is fixed to five voting nodes.
- The current implementation hosts those five nodes inside one process through `TestCluster`.
- Client traffic enters through one gRPC listener per `kvd` process.
- Peer replication is in-process through `TestTransport`, not a separate networked Raft RPC layer.
- Persistent state is stored on local disk beneath the configured `--data_dir`, with one subdirectory per logical node.
- Majority availability is required for acknowledged writes and for linearizable reads.
- Client-facing TLS can run in `dev` or `secure` mode. Inter-node traffic has no separate TLS layer because it is not a separate network path in the current implementation.

This topology is sufficient for the validated v1 behavior, but it should be read as an implementation fact of the current tree, not as a claim that runtime membership changes or multi-process peer transport exist.

## Deferred Scope and Non-Goals

The following items remain out of scope for v1 and are not implemented in the current tree:

- `Txn` and multi-operation atomic APIs
- `Watch` and `Lease` APIs
- `RangeScan` and secondary-index capabilities
- Dynamic membership add, remove, or reconfigure flows
- Multi-region replication

These exclusions are frozen by `docs/architecture/scope.md` and `docs/adr/0004-deferred-scope-v1-boundary.md`.

## ADR Mapping

| ADR | Decision carried into this architecture | Concrete impact |
|---|---|---|
| `docs/adr/0001-consistency-model-linearizable-v1.md` | Linearizable reads are the v1 default | `KvRaftService::Get` requires leader ownership and quorum contact before reading |
| `docs/adr/0002-static-membership-five-node-v1.md` | Membership is static with five voting nodes | `TestCluster` defaults to five node ids and exposes no runtime membership mutation API |
| `docs/adr/0003-integrity-checksum-strategy-v1.md` | WAL and SST checksum verification is mandatory on replay and read | `wal.cpp` and `sstable.cpp` reject corruption before data is accepted |
| `docs/adr/0004-deferred-scope-v1-boundary.md` | Non-core APIs and topology changes stay out of v1 | `kv.proto` exposes only `Put`, `Get`, `Delete`; no membership, watch, lease, txn, or range APIs exist |

## Validation and Evidence

### Code evidence

- API contract: `proto/kvstore/v1/kv.proto`
- Server bootstrap and TLS profile handling: `src/bin/kvd.cpp`
- gRPC adapter: `src/api/grpc_kv_service.cpp`
- Request validation, idempotency, leader gating, and commit waiting: `src/service/kv_raft_service.cpp`
- Raft election, replication, quorum contact, and commit advancement: `src/raft/raft_node.cpp`
- In-process five-node transport and node availability control: `src/raft/test_transport.cpp`
- WAL durability and replay verification: `src/engine/wal.cpp`
- Engine orchestration, flush, compaction, and per-node storage layout: `src/engine/kv_engine.cpp`
- SST read and checksum verification: `src/engine/sstable.cpp`
- On-disk format summary: `docs/storage-format.md`

### Runtime evidence from Task 7

- Leader failover: `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`
  - Confirms pass, leader replacement, and failover within the documented threshold.
- Restart recovery time: `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`
  - Confirms restart recovery within the v1 acceptance window.
- Partition healing and TLS profile coverage: `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
  - Confirms partition-time write rejection, post-heal recovery, and both `dev` and `secure` TLS profiles.
- Supporting logs for the same run family: `.sisyphus/evidence/task7-suite/task-7-partition-heal.log`, `.sisyphus/evidence/task7-suite/task-7-tls-toggle.log`
- Integrity corruption detection: `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`
  - Confirms WAL and SST corruption are detected as `CHECKSUM_MISMATCH`, not accepted silently.
- Benchmark and acknowledged-write-loss gate: `.sisyphus/evidence/task7-post-fix/task-7-bench.json`
  - Confirms `samples=300`, `p99_durable_write_ms=1.416`, `p99_read_ms=0.002`, and `no_acknowledged_write_loss=true`.

### Evidence note

`.sisyphus/evidence/task7-checks/bench/task-7-bench.json` records an earlier failed benchmark attempt with `{"pass":false,"reason":"acknowledged_write_lost"}`. The current architecture references `.sisyphus/evidence/task7-post-fix/task-7-bench.json` as the authoritative post-fix benchmark artifact for Task 7.
