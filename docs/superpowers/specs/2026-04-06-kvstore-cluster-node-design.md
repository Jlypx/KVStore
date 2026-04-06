# KVStore Multi-Process Cluster-Node Design

## Purpose

This design defines how KVStore evolves from the current single-process embedded Raft topology into a same-host, multi-process, real five-node cluster while preserving the existing embedded mode for tests and local development.

The design assumes the following decisions are already made:

- runtime shape: same-host true cluster
- node count: static five-node cluster
- networking model: each node exposes both a client port and a peer port
- compatibility: keep the current embedded mode as a supported path
- implementation strategy: layered extension, not a full rewrite

This is a design for the next large architecture step. It does not attempt to implement dynamic membership, cross-region routing, or a production deployment platform in the same pass.

## Current State

Today, KVStore has these important properties:

- `kvd` starts one process
- that process embeds a deterministic five-node Raft cluster
- peer messaging is in-process through `TestTransport`
- the client-visible gRPC surface is `Put`, `Get`, `Delete`
- service semantics already enforce leader-only writes, leader-only linearizable reads, idempotency, and commit-before-ack behavior
- the storage layer already has WAL, tombstone-aware SSTs, compaction, and WAL generation handling
- Raft metadata and log are now durably persisted locally

That means the biggest missing piece is no longer storage or protocol semantics. The missing piece is turning "five logical nodes in one address space" into "five real nodes with real peer networking and process boundaries."

## Goals

- Add a real multi-process five-node runtime on one host.
- Preserve the existing embedded mode as a compatibility and test mode.
- Keep the client-visible API unchanged: still unary `Put/Get/Delete`.
- Introduce a real peer RPC layer for Raft traffic between processes.
- Keep node membership static at five nodes for this phase.
- Allow clients to connect to any node's client endpoint and receive redirect-style not-leader behavior.
- Add local launcher and integration coverage for same-host five-node operation.
- Keep the current service and engine semantics as unchanged as possible.

## Non-Goals

- No dynamic membership add/remove/reconfigure.
- No snapshot/install-snapshot in this phase.
- No cross-machine orchestration requirements.
- No Kubernetes/systemd/docker production packaging in this phase.
- No watch/lease/txn/range APIs.
- No automatic client-side smart routing library.

## Recommended Approach

### Rejected Approach A: Replace the embedded system outright

This would remove the current in-process cluster and force the entire repository onto the new runtime immediately.

Why reject it:

- breaks too many existing tests at once
- removes a deterministic protocol testbed that is still valuable
- makes migration much riskier

### Rejected Approach B: Keep one external entrypoint and hide the cluster behind an internal gateway

This would create a "real" cluster internally but keep one externally visible gateway as the only client endpoint.

Why reject it:

- less faithful to a real distributed node model
- pushes leader semantics into an extra layer that we do not need yet
- complicates reasoning about where redirection and consistency decisions live

### Recommended Approach C: Dual runtime modes with explicit cluster-node runtime

Keep the current embedded runtime and add a second runtime where one `kvd` process equals one Raft node.

Why this is best:

- preserves current deterministic test coverage
- isolates network/runtime complexity to new modules
- reuses `RaftNode`, `KvEngine`, and most service semantics
- keeps migration incremental and reviewable

## Runtime Modes

`kvd` gains an explicit `--mode=` selector.

### `--mode=embedded`

This remains the current behavior:

- one process
- embedded five-node cluster
- in-memory peer transport
- existing tests continue to target this path unless explicitly migrated

### `--mode=cluster-node`

This is the new runtime:

- one process equals one Raft node
- one local `KvEngine`
- one local `RaftNode`
- one client listener for external requests
- one peer listener for internal Raft RPC
- static peer list loaded from config

The two modes must share as much code as possible below the runtime boundary.

## Node Architecture

In `cluster-node` mode, each node process contains:

- `GrpcKvService` on the client-facing listener
- `RaftPeerService` on the peer listener
- `KvRaftService` or an equivalent service-layer coordinator
- one local `RaftNode`
- one local `KvEngine`
- one network transport implementation for peer RPC
- one background ticker thread for Raft timing

### Internal Thread Model

To satisfy the "multi-threaded true cluster" goal without overcomplicating the design, each node process should use a small, explicit threading model:

- **gRPC server threads**
  - provided by gRPC runtime for client and peer listeners
- **Raft ticker thread**
  - advances time, sends heartbeats, triggers elections
- **optional apply/commit handoff thread**
  - not required initially if commit callbacks stay serialized under service/runtime coordination

The first version should keep state ownership simple:

- `RaftNode` access is serialized behind runtime/service locks
- network callbacks do not mutate Raft state concurrently without synchronization
- no attempt to fully parallelize internal state-machine apply in phase 1

This gives us true multi-threading per process while keeping the correctness model understandable.

## Networking Model

Each node has two addresses:

- `client_addr`
  - handles `Put/Get/Delete`
- `peer_addr`
  - handles Raft internal RPC only

### Why separate client and peer listeners

- cleaner security and observability boundaries
- avoids mixing public API and internal protocol handlers
- easier future evolution if peer RPC needs different timeouts, auth, or metrics

## Configuration Model

The cluster-node runtime uses a static configuration file.

### Configuration choice

Use a small repo-local text format rather than a large config subsystem. The simplest practical choice is line-oriented key-value plus repeated node blocks, or TOML-like sections if we want slightly better structure.

The first version should support this information:

- `cluster_id`
- `self_id`
- `tls_profile` for client listener
- `client_addr`
- `peer_addr`
- `data_dir`
- full static member list of all five nodes:
  - `node_id`
  - `client_addr`
  - `peer_addr`

### Recommended example shape

```toml
cluster_id = "kvstore-local-1"
self_id = 1
tls_profile = "dev"
data_dir = "./data/node1"
client_addr = "127.0.0.1:50051"
peer_addr = "127.0.0.1:60051"

[[nodes]]
node_id = 1
client_addr = "127.0.0.1:50051"
peer_addr = "127.0.0.1:60051"

[[nodes]]
node_id = 2
client_addr = "127.0.0.1:50052"
peer_addr = "127.0.0.1:60052"

# ...
```

### Validation rules

- exactly five unique `node_id`s
- `self_id` must exist in member list
- no duplicate `client_addr`
- no duplicate `peer_addr`
- peer addresses must not equal client addresses for the same node

## Peer RPC Design

The repository needs a new internal protobuf contract, separate from the public KV API.

### New protobuf

Add a new proto, for example:

- `proto/kvstore/v1/raft.proto`

This proto should define:

- `RequestVote`
- `AppendEntries`
- their responses

The fields map closely to the current in-memory Raft message types:

- `term`
- `candidate_id`
- `last_log_index`
- `last_log_term`
- `leader_id`
- `prev_log_index`
- `prev_log_term`
- repeated log entries
- `leader_commit`
- `success`
- `match_index`

### New internal gRPC service

Add a peer-only service, for example:

```proto
service RaftPeer {
  rpc RequestVote(RequestVoteRequest) returns (RequestVoteResponse);
  rpc AppendEntries(AppendEntriesRequest) returns (AppendEntriesResponse);
}
```

This service is not exposed to clients and should not be treated as public API.

## Transport Abstraction

The current `TestTransport` proves the repository already has an implicit transport boundary. We should make that boundary explicit.

### Recommended abstraction

Introduce a transport interface with two implementations:

- `InProcessTransport`
  - wraps or replaces current test transport behavior
- `NetworkTransport`
  - sends RPCs over the peer gRPC service

`RaftNode` itself should still operate on Raft messages, not on gRPC types.

The transport layer is responsible for:

- converting between `RaftNode` message structs and protobuf messages
- best-effort send behavior
- surfacing response messages back into the local node runtime

## Service-Layer Behavior in Cluster Mode

The cluster-node mode should preserve current semantics wherever possible.

### Write requests

- accepted only by leader
- rejected by followers with not-leader information
- acknowledged only after commit and local apply result is ready

### Read requests

- still leader-only for phase 1
- still require quorum contact

### Redirect behavior

On a follower:

- return `FAILED_PRECONDITION`
- include `leader_hint=<node_id>` as today

We can optionally enrich this later with the leader's client address, but phase 1 does not need a richer redirect contract if the node IDs and config are stable.

## Leader Hint Strategy

There are two options:

### Option 1: Keep leader hint as node ID only

Pros:

- fully compatible with current behavior
- minimal code churn

Cons:

- clients need their own mapping from node ID to client address

### Option 2: Return node ID and leader client address

Pros:

- better UX for external clients

Cons:

- expands the wire-visible behavior

### Recommendation

Phase 1 should keep the existing node-ID hint behavior in the public API and document how launch configs map IDs to addresses. If needed, structured redirect metadata can be a later enhancement.

## Storage Layout in Cluster Mode

In cluster-node mode, each process owns only its own local storage.

Recommended layout:

- `data_dir/engine/...`
- `data_dir/raft/...`

This keeps the separation introduced by recent hardening work:

- engine WAL / SST state for the state machine
- raft metadata / log state for the protocol

Unlike embedded mode, a node process must not create sibling directories for the other four nodes.

## `kvd` CLI Changes

`kvd` should evolve from the current small argument parser into a mode-aware parser.

### Embedded mode flags

Preserve current flags:

- `--listen_addr=`
- `--data_dir=`
- `--tls_profile=`
- `--tls_cert=`
- `--tls_key=`

### Cluster-node mode flags

Add:

- `--mode=cluster-node`
- `--config=PATH`

Embedded remains the default for compatibility in the first transition phase:

```bash
kvd --mode=embedded ...
```

or just current behavior if no mode is given.

## Deployment and Launching

Phase 1 should add a same-host launcher, not a full deployment system.

### Deliverables

- five sample config files in `deploy/`
- a local launch script in `scripts/` or `deploy/`
- a matching stop/cleanup helper if practical

Example launcher responsibilities:

- start five `kvd --mode=cluster-node` processes
- assign config files
- redirect logs to per-node files
- optionally wait for a leader to appear

This is a development launcher, not production orchestration.

## Testing Strategy

This migration needs layered validation, not one giant integration test.

### Layer 1: Existing embedded tests stay intact

Current embedded tests should remain green. This is the compatibility guarantee.

### Layer 2: Peer RPC serialization tests

Add targeted tests for:

- protobuf <-> internal Raft type conversion
- `RequestVote` request/response mapping
- `AppendEntries` request/response mapping

### Layer 3: Single-node runtime tests

Add tests for:

- config parsing
- invalid config rejection
- node startup with client and peer listeners

### Layer 4: Same-host five-node cluster integration tests

Add new integration tests that:

- start five real node processes on one host
- wait for leader election
- send client RPCs to arbitrary nodes
- verify not-leader responses and leader success
- verify replication, failover, and recovery across processes

### Layer 5: Launcher-driven smoke tests

Add a launcher-based script that:

- starts five nodes
- waits for readiness
- runs a small client workload
- kills the leader
- verifies new leader election and successful writes

## Migration Plan

This must be phased so embedded mode never breaks while cluster mode is being built.

### Phase A: Introduce transport abstraction and peer proto

- add `raft.proto`
- add peer service definitions
- add conversion helpers
- keep embedded mode as the only active runtime

### Phase B: Add network transport and cluster-node runtime

- implement peer RPC client/server
- implement node config parsing
- start one node process with client and peer listeners

### Phase C: Add five-node same-host cluster launcher and tests

- static five-node config set
- local launcher
- real multi-process integration coverage

### Phase D: Improve operability

- better logs
- readiness checks
- stronger local smoke tooling

## Minimal First Deliverable

The first acceptable deliverable for this design is:

- `embedded` mode still passes current tests
- `cluster-node` mode exists
- five same-host node processes can be started from static configs
- nodes communicate over real peer gRPC
- clients can talk to any node's client endpoint
- follower writes are rejected with leader hint
- leader writes commit successfully across the real multi-process cluster
- leader failover works after killing one process

If all of the above work, the repository has crossed the boundary from embedded cluster simulation into a real local cluster runtime.

## Risks and Mitigations

### Risk: Too much logic leaks into `kvd.cpp`

Mitigation:

- move runtime construction into dedicated runtime classes
- keep `kvd.cpp` as argument parsing plus mode dispatch

### Risk: Embedded and cluster-node modes diverge semantically

Mitigation:

- share `RaftNode`, `KvEngine`, and service semantics
- keep differences constrained to runtime and transport

### Risk: Network transport introduces race conditions

Mitigation:

- keep a clear serialized state-access model inside each node
- use explicit locking around Raft state transitions
- start with correctness before optimization

### Risk: Configuration and launch complexity hide bugs

Mitigation:

- make config validation strict
- provide sample configs checked into the repo
- add launcher-driven smoke tests

### Risk: This becomes a productionization rabbit hole

Mitigation:

- explicitly stop at same-host static five-node runtime for phase 1
- no dynamic membership
- no orchestrator
- no snapshotting in this pass

## Deliverables Added by This Design

At the end of the full implementation of this design, the repository should contain:

- a new cluster-node runtime mode
- a peer protobuf and peer gRPC service
- a network transport implementation
- static five-node config files
- same-host launcher scripts
- multi-process cluster integration tests
- preserved embedded-mode compatibility

## Recommended Next Implementation Plan

The implementation plan should break this design into at least these milestone commits:

1. `feat(raft): add peer rpc contract and transport abstraction`
2. `feat(runtime): add cluster-node mode and static config loading`
3. `feat(cluster): add same-host five-node launcher and peer networking`
4. `test(cluster): add real multi-process failover and client-routing coverage`

This ordering keeps the migration incremental and keeps the embedded path valid throughout.
