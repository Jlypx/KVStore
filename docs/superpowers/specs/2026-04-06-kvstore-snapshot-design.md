# KVStore Snapshot Design

## Purpose

This design adds the missing snapshot path to the current KVStore implementation.
The current tree already has durable Raft metadata and log persistence, same-host multi-process peer RPC, and a real five-node local cluster mode, but it still does not support:

- local Raft log compaction through snapshots
- `InstallSnapshot` RPC between peers
- follower catch-up through snapshot transfer when log suffix replay is no longer sufficient

This design focuses on a minimal, correct snapshot system that fits the repository as it exists today.

## Goals

- Add snapshot metadata to durable Raft storage.
- Add `InstallSnapshot` to the internal peer protocol.
- Allow a node to compact its Raft log after enough committed entries accumulate.
- Allow a lagging follower to catch up through snapshot installation.
- Keep engine state and Raft snapshot metadata consistent.
- Reuse the current engine, SST, and transport layers as much as possible.

## Non-Goals

- No streaming chunked snapshot protocol in this pass.
- No dynamic membership.
- No cross-region snapshot orchestration.
- No production-grade compression or incremental snapshots.

## Snapshot Model

The minimal repository-appropriate snapshot model is:

1. The state machine snapshot is a full copy of the current logical KV state plus request-id dedup state.
2. The Raft snapshot metadata stores:
   - `last_included_index`
   - `last_included_term`
3. After snapshot creation, the local Raft log is truncated up to the snapshot boundary.
4. If a leader sees that a follower is behind the snapshot base, it sends an `InstallSnapshot` RPC instead of more `AppendEntries`.

This is a full-state snapshot model, not an incremental or chunked model.

## Engine Snapshot Design

The engine already has enough primitives to support snapshot export/import:

- it can read the full logical state through memtable + SST layers
- it already persists key/value state and tombstones
- it already tracks request-id dedup state through the memtable

### Snapshot payload contents

The snapshot payload must include:

- all currently live KV pairs
- all request IDs needed to preserve idempotency behavior after install

Deleted keys do not need tombstones in the snapshot payload because a snapshot represents the full replacement state; absent keys are simply absent.

### Snapshot serialization

Use a compact repo-local binary format:

- `u32 magic`
- `u16 version`
- `u32 kv_count`
- repeated:
  - `u32 key_size`
  - `u32 value_size`
  - `key`
  - `value`
- `u32 request_id_count`
- repeated:
  - `u32 request_id_size`
  - `request_id`

This keeps snapshot export/import independent from SST on-disk format details.

### Engine import behavior

Installing a snapshot into the engine should:

1. stop using the current memtable and active WAL writer
2. clear current WAL generations and SST files
3. write one fresh SST representing the snapshot contents
4. reset the active WAL generation
5. rebuild the in-memory request-id set from the snapshot payload

After install, the engine's logical state should exactly match the snapshot contents.

## Raft Metadata Design

### New persisted fields

Extend durable Raft state with:

- `snapshot_last_included_index`
- `snapshot_last_included_term`

The persisted log then becomes "the suffix after snapshot".

### Logical log indexing

The current `RaftNode` assumes `log_[index]` works directly for logical indices.
Snapshot support requires a log base offset.

Introduce:

- `log_base_index_`
- `log_base_term_`

and interpret the in-memory log vector as:

- `log_[0]` is a dummy/base entry representing the snapshot boundary
- logical index of `log_[0]` equals `log_base_index_`

All direct log indexing must be converted through helper translation:

- logical index -> vector offset

This is the central mechanical change required for snapshots.

## InstallSnapshot RPC Design

Extend `proto/kvstore/v1/raft.proto` with:

- `InstallSnapshotRequest`
- `InstallSnapshotResponse`

Recommended shape:

```proto
message InstallSnapshotRequest {
  uint64 term = 1;
  uint32 leader_id = 2;
  uint64 last_included_index = 3;
  uint64 last_included_term = 4;
  bytes snapshot_payload = 5;
}

message InstallSnapshotResponse {
  uint64 term = 1;
  bool success = 2;
  uint64 last_included_index = 3;
}
```

This remains unary for now. The tests and local same-host topology do not need chunked streaming yet.

## Runtime and Service Integration

### Local snapshot creation

Snapshot creation should not happen inside `RaftNode` directly because the node does not own state-machine storage.

Instead:

- `RaftNode` exposes when it is snapshot-eligible
- `KvRaftService` and `ClusterNodeService` coordinate snapshot creation after committed/apply progress

Recommended rule:

- if `commit_index - log_base_index >= snapshot_threshold_entries`, trigger snapshot creation

### Local snapshot application

The service layer performs:

1. export engine snapshot payload
2. call a RaftNode method to install the new local snapshot metadata and truncate log prefix

### Follower snapshot install

When `RaftNode::LeaderSendAppendEntries` sees:

- `next_index_[follower] <= log_base_index_`

it should request snapshot send instead of emitting `AppendEntries`.

The service/runtime snapshot-send hook then:

1. exports snapshot payload from the local engine
2. sends `InstallSnapshotRequest` through the transport

On the follower:

1. peer service receives `InstallSnapshotRequest`
2. service installs snapshot payload into the engine
3. service calls `RaftNode` to install snapshot metadata and truncate local log state
4. response returns success to the leader

## Transport Integration

The transport and peer service layers must be extended to handle:

- `InstallSnapshotRequest`
- `InstallSnapshotResponse`

This affects:

- `raft_types.h`
- `raft.proto`
- proto conversion helpers
- `RaftPeerService`
- `NetworkTransport`
- in-process transport path used by embedded tests

## Testing Strategy

### Layer 1: Raft storage round-trip

Extend persistence tests to verify:

- snapshot metadata round-trips through `RaftStorage`

### Layer 2: Engine snapshot export/import

Add tests that:

- write keys and deletes
- export snapshot
- install snapshot into a fresh engine
- verify final state and request-id behavior

### Layer 3: Local Raft snapshot compaction

Add deterministic tests showing:

- after enough committed entries, snapshot is created
- log base advances
- older entries are compacted out of the local suffix

### Layer 4: Snapshot catch-up

Add a deterministic embedded-cluster test:

- take one follower down
- advance leader enough to trigger snapshot and log truncation
- bring follower back
- verify follower catches up through `InstallSnapshot`

### Layer 5: Optional cluster-node smoke follow-up

If practical in this pass, add one same-host multi-process check that confirms:

- a lagging process can recover through snapshot install

Deterministic embedded coverage is the minimum acceptable proof in this pass.

## Risks

### Risk: Log indexing bugs

Snapshot support requires base-index-aware indexing. This is the highest-risk part.

Mitigation:

- add helper functions for logical index translation
- remove direct assumptions that `log_[index]` means logical index
- test both pre-snapshot and post-snapshot access paths

### Risk: Engine and Raft snapshot state drift apart

Mitigation:

- keep snapshot creation coordinated in the service layer
- engine import must complete before snapshot metadata is accepted on follower install

### Risk: Losing idempotency semantics after snapshot install

Mitigation:

- include request-id history in snapshot payload
- add explicit tests for duplicate request behavior after install

## Deliverable

At the end of this work, KVStore should support:

- local snapshot creation
- Raft log truncation after snapshot
- snapshot metadata persistence across restart
- `InstallSnapshot` peer RPC
- follower catch-up through snapshot when it falls behind the truncated log
