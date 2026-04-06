# KVStore Cluster-Node Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a same-host, multi-process, true five-node cluster runtime while preserving the existing embedded mode and client-visible API.

**Architecture:** The implementation keeps `embedded` mode intact and adds a new `cluster-node` runtime with separate client and peer listeners. The migration is staged: first add peer RPC and transport boundaries, then add config-aware cluster-node runtime, then add launcher and integration coverage. `RaftNode`, `KvEngine`, and service semantics remain shared; runtime wiring and transport diverge by mode.

**Tech Stack:** C++20, CMake, gRPC/protobuf, current KVStore Raft/engine/service modules, WSL launcher scripts, deterministic and process-level tests

---

## File Map

- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\proto\kvstore\v1\raft.proto`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\CMakeLists.txt`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\cmake\kvstore_grpc.cmake`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_transport.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\inprocess_transport.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\network_transport.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\inprocess_transport.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\network_transport.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\service\raft_peer_service.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\raft_peer_service.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\runtime\cluster_config.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\runtime\cluster_config.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\runtime\embedded_runtime.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\runtime\embedded_runtime.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\runtime\cluster_node_runtime.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\runtime\cluster_node_runtime.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_node.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_node.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\service\kv_raft_service.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\kv_raft_service.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\bin\kvd.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_peer_proto_test.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\cluster_config_test.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\cluster_node_smoke_test.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\integration\multi_process_cluster_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\CMakeLists.txt`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\integration\CMakeLists.txt`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_1.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_2.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_3.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_4.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_5.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\cluster\start_local_cluster.sh`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\cluster\stop_local_cluster.sh`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\README.md`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\architecture.md`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\operations.md`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\wire-protocol.md`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\testing.md`

### Task 1: Add the Peer RPC Contract

**Files:**
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\proto\kvstore\v1\raft.proto`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\CMakeLists.txt`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_peer_proto_test.cpp`

- [ ] **Step 1: Write a failing test that round-trips Raft peer request/response fields through conversion helpers**

Create `tests/raft_peer_proto_test.cpp` with checks for:

```cpp
// RequestVote: term, candidate_id, last_log_index, last_log_term
// AppendEntries: term, leader_id, prev_log_index, prev_log_term, leader_commit
// LogEntry vector: term + command
// AppendEntriesResponse: term, success, match_index
```

The test must compare internal Raft structs against converted proto-backed equivalents once the helpers exist.

- [ ] **Step 2: Run the new test target to confirm it fails**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j2 --target kvstore_raft_peer_proto_test && cd build && ctest --output-on-failure -R kvstore_raft_peer_proto_test"
```

Expected: FAIL because `raft.proto` and conversion helpers do not exist yet.

- [ ] **Step 3: Add `proto/kvstore/v1/raft.proto` with internal peer RPC messages and service**

Define:

```proto
syntax = "proto3";

package kvstore.v1;

service RaftPeer {
  rpc RequestVote(RequestVoteRequest) returns (RequestVoteResponse);
  rpc AppendEntries(AppendEntriesRequest) returns (AppendEntriesResponse);
}

message LogEntry {
  uint64 term = 1;
  bytes command = 2;
}

message RequestVoteRequest {
  uint64 term = 1;
  uint32 candidate_id = 2;
  uint64 last_log_index = 3;
  uint64 last_log_term = 4;
}

message RequestVoteResponse {
  uint64 term = 1;
  bool vote_granted = 2;
}

message AppendEntriesRequest {
  uint64 term = 1;
  uint32 leader_id = 2;
  uint64 prev_log_index = 3;
  uint64 prev_log_term = 4;
  repeated LogEntry entries = 5;
  uint64 leader_commit = 6;
}

message AppendEntriesResponse {
  uint64 term = 1;
  bool success = 2;
  uint64 match_index = 3;
}
```

- [ ] **Step 4: Extend protobuf generation in `src/CMakeLists.txt` to compile `raft.proto`**

Add generation outputs and a `kvstore_raft_proto` target alongside the current KV proto build.

- [ ] **Step 5: Add conversion helpers and make the test pass**

Add helper functions in a new transport-facing module to convert:

```cpp
kvstore::raft::RequestVoteRequest <-> kvstore::v1::RequestVoteRequest
kvstore::raft::RequestVoteResponse <-> kvstore::v1::RequestVoteResponse
kvstore::raft::AppendEntriesRequest <-> kvstore::v1::AppendEntriesRequest
kvstore::raft::AppendEntriesResponse <-> kvstore::v1::AppendEntriesResponse
kvstore::raft::LogEntry <-> kvstore::v1::LogEntry
```

- [ ] **Step 6: Run the peer proto test again**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_raft_peer_proto_test"
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add proto/kvstore/v1/raft.proto src/CMakeLists.txt tests/raft_peer_proto_test.cpp include/kvstore/raft/raft_transport.h include/kvstore/raft/inprocess_transport.h include/kvstore/raft/network_transport.h src/raft/inprocess_transport.cpp src/raft/network_transport.cpp
git commit -m "feat(raft): add peer rpc contract and transport abstraction"
```

### Task 2: Introduce Transport Abstraction Without Breaking Embedded Mode

**Files:**
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_transport.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\inprocess_transport.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\inprocess_transport.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\test_transport.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\test_transport.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\cluster_runtime.cpp`
- Test: existing embedded Raft tests

- [ ] **Step 1: Write a failing embedded compatibility check**

Use the existing embedded transport tests:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(raft_election|raft_replication|raft_quorum|raft_failover)_test'"
```

Expected: they pass now; after introducing abstraction, re-run to guarantee no regression.

- [ ] **Step 2: Add a transport interface that works in terms of internal Raft messages**

Define an interface equivalent to:

```cpp
class RaftTransport {
 public:
  virtual ~RaftTransport() = default;
  virtual auto RegisterNode(NodeId id, std::function<void(Message)> handler) -> void = 0;
  virtual auto Send(Message message) -> void = 0;
};
```

- [ ] **Step 3: Rework the existing in-process transport to satisfy that interface**

Move the current queueing/delivery semantics into an `InProcessTransport` implementation and keep deterministic delivery behavior for embedded tests.

- [ ] **Step 4: Rewire embedded runtime to use the transport abstraction**

Do not change embedded semantics. `embedded` mode must still pass all current Raft tests.

- [ ] **Step 5: Re-run embedded Raft tests**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(raft_election|raft_replication|raft_quorum|raft_failover)_test'"
```

Expected: PASS.

### Task 3: Add Cluster Configuration Parsing

**Files:**
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\runtime\cluster_config.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\runtime\cluster_config.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\cluster_config_test.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_1.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_2.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_3.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_4.toml`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\kvd_cluster_node_5.toml`

- [ ] **Step 1: Write a failing config parsing test**

Create `tests/cluster_config_test.cpp` with checks for:

```cpp
// loads self_id, cluster_id, tls_profile, data_dir, client_addr, peer_addr
// loads exactly five nodes
// rejects duplicate node ids
// rejects duplicate client or peer addresses
// rejects self_id missing from member list
```

- [ ] **Step 2: Run the config test to verify it fails**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && cmake --build build -j2 --target kvstore_cluster_config_test && cd build && ctest --output-on-failure -R kvstore_cluster_config_test"
```

Expected: FAIL because config parser does not exist.

- [ ] **Step 3: Implement a minimal static config loader**

Add types equivalent to:

```cpp
struct ClusterNodeConfig {
  NodeId node_id;
  std::string client_addr;
  std::string peer_addr;
};

struct ClusterProcessConfig {
  std::string cluster_id;
  NodeId self_id;
  std::string tls_profile;
  std::filesystem::path data_dir;
  std::string client_addr;
  std::string peer_addr;
  std::vector<ClusterNodeConfig> nodes;
};
```

Use a small parser for the chosen config format and enforce validation rules from the spec.

- [ ] **Step 4: Add five sample local configs in `deploy/`**

Use a static five-node loopback topology:

```toml
node1: client 127.0.0.1:50051, peer 127.0.0.1:60051
node2: client 127.0.0.1:50052, peer 127.0.0.1:60052
...
node5: client 127.0.0.1:50055, peer 127.0.0.1:60055
```

- [ ] **Step 5: Run the config test again**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_cluster_config_test"
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/kvstore/runtime/cluster_config.h src/runtime/cluster_config.cpp tests/cluster_config_test.cpp deploy/kvd_cluster_node_1.toml deploy/kvd_cluster_node_2.toml deploy/kvd_cluster_node_3.toml deploy/kvd_cluster_node_4.toml deploy/kvd_cluster_node_5.toml
git commit -m "feat(runtime): add cluster-node mode and static config loading"
```

### Task 4: Add Peer Service and Network Transport

**Files:**
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\service\raft_peer_service.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\raft_peer_service.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\network_transport.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\network_transport.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_node.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_node.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\cluster_node_smoke_test.cpp`

- [ ] **Step 1: Write a failing peer transport smoke test**

Create `tests/cluster_node_smoke_test.cpp` with a minimal same-process network loopback harness or a two-node peer service harness that verifies:

```cpp
// a RequestVote request sent through NetworkTransport reaches a peer service
// the response converts back into an internal Raft message correctly
```

- [ ] **Step 2: Run the smoke test to verify it fails**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && cmake --build build -j2 --target kvstore_cluster_node_smoke_test && cd build && ctest --output-on-failure -R kvstore_cluster_node_smoke_test"
```

Expected: FAIL because peer service and network transport do not exist.

- [ ] **Step 3: Implement `RaftPeerService`**

This service must:

```cpp
// receive RequestVote / AppendEntries protobufs
// convert to internal Raft message types
// submit them into the local node/runtime
// convert the response back to protobuf
```

Do not expose this service on the client listener.

- [ ] **Step 4: Implement `NetworkTransport`**

This transport must:

```cpp
// map NodeId -> peer_addr from config
// send peer gRPC requests
// deliver responses back into the local runtime in internal message form
```

- [ ] **Step 5: Re-run the cluster-node smoke test**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_cluster_node_smoke_test"
```

Expected: PASS.

### Task 5: Add the `cluster-node` Runtime

**Files:**
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\runtime\embedded_runtime.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\runtime\embedded_runtime.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\runtime\cluster_node_runtime.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\runtime\cluster_node_runtime.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\service\kv_raft_service.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\kv_raft_service.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\bin\kvd.cpp`

- [ ] **Step 1: Write a failing mode-selection test**

Extend `tests/cluster_node_smoke_test.cpp` or add a focused runtime test that verifies:

```cpp
// kvd --mode=embedded still creates the current embedded runtime
// kvd --mode=cluster-node --config=... constructs a single-node runtime with separate client and peer listeners
```

- [ ] **Step 2: Run the runtime smoke test to verify it fails**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_cluster_node_smoke_test"
```

Expected: FAIL because `kvd` has no mode-aware runtime dispatch.

- [ ] **Step 3: Split runtime wiring out of `kvd.cpp`**

Introduce dedicated runtime constructors so `kvd.cpp` only:

```cpp
// parses args
// selects embedded vs cluster-node mode
// constructs the corresponding runtime
// starts listeners
```

- [ ] **Step 4: Implement `cluster-node` runtime**

For one node process:

```cpp
// load config
// build local KvEngine rooted at data_dir/engine
// build local RaftNode rooted at data_dir/raft
// attach NetworkTransport
// start client gRPC listener on client_addr
// start peer gRPC listener on peer_addr
// run ticker thread
```

- [ ] **Step 5: Preserve `embedded` mode**

The existing embedded behavior must still work with the same single-process tests.

- [ ] **Step 6: Run embedded and cluster-node smoke slices**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(smoke|raft_failover|cluster_node_smoke)_test'"
```

Expected: PASS.

### Task 6: Add Same-Host Launcher and Real Multi-Process Integration Test

**Files:**
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\cluster\start_local_cluster.sh`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\cluster\stop_local_cluster.sh`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\integration\multi_process_cluster_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\integration\CMakeLists.txt`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\deploy\README.md`

- [ ] **Step 1: Write a failing multi-process integration test**

Create `tests/integration/multi_process_cluster_test.cpp` that:

```cpp
// starts five kvd --mode=cluster-node processes from deploy configs
// waits for leader election
// sends Put/Get/Delete requests to arbitrary client endpoints
// asserts non-leader writes fail with FAILED_PRECONDITION
// finds a leader endpoint, verifies successful write and read
// kills the leader process
// waits for new leader
// verifies post-failover write succeeds
```

- [ ] **Step 2: Run the test to confirm it fails**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && cmake --build build -j2 --target kvstore_multi_process_cluster_test && cd build && ctest --output-on-failure -R kvstore_multi_process_cluster_test"
```

Expected: FAIL because launcher/process runtime support is incomplete.

- [ ] **Step 3: Implement local cluster launcher scripts**

`start_local_cluster.sh` must:

```bash
# start 5 node processes
# each with its own config and log file
# write pid files
# optionally wait until one node reports leader-ready
```

`stop_local_cluster.sh` must:

```bash
# stop all pid-file-backed processes
# clean transient run artifacts if requested
```

- [ ] **Step 4: Make the multi-process integration test pass**

Use the launcher plus real client RPCs to prove:

```cpp
// same-host true cluster exists
// leader election works
// redirection semantics work
// failover across real processes works
```

- [ ] **Step 5: Re-run the integration test**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_multi_process_cluster_test"
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/kvstore/service/raft_peer_service.h src/service/raft_peer_service.cpp include/kvstore/runtime/embedded_runtime.h src/runtime/embedded_runtime.cpp include/kvstore/runtime/cluster_node_runtime.h src/runtime/cluster_node_runtime.cpp src/bin/kvd.cpp tests/cluster_node_smoke_test.cpp tests/integration/multi_process_cluster_test.cpp tests/integration/CMakeLists.txt scripts/cluster/start_local_cluster.sh scripts/cluster/stop_local_cluster.sh deploy/README.md
git commit -m "feat(cluster): add same-host five-node cluster runtime"
```

### Task 7: Documentation and Final Verification

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\architecture.md`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\operations.md`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\wire-protocol.md`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\docs\testing.md`

- [ ] **Step 1: Update architecture and operations docs**

Document:

```markdown
- embedded vs cluster-node mode
- client_addr vs peer_addr
- static five-node config
- same-host launcher flow
```

- [ ] **Step 2: Update wire protocol and testing docs**

Document:

```markdown
- public API unchanged
- peer RPC is internal
- new multi-process integration coverage
```

- [ ] **Step 3: Run the main verification suite**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure"
```

Expected: PASS.

- [ ] **Step 4: Run the release/docs readiness gate if applicable**

Run:

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && bash scripts/release/check_release_readiness.sh"
```

Expected: PASS, or document exact failures if unrelated to this feature.

- [ ] **Step 5: Inspect working tree**

Run:

```bash
git status --short
```

Expected: clean working tree.

- [ ] **Step 6: Commit docs if changed separately**

```bash
git add docs/architecture.md docs/operations.md docs/wire-protocol.md docs/testing.md
git commit -m "docs: describe cluster-node runtime and multi-process testing"
```

## Plan Review Notes

- Spec coverage is complete for runtime modes, peer RPC, config loading, launcher, same-host five-node operation, and compatibility with embedded mode.
- Dynamic membership, snapshots, and production deployment automation remain deliberately out of scope.
- The user explicitly asked to start immediately, so inline execution is the default after this plan is committed.
