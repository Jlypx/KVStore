# KVStore Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Repair the WSL worktree baseline and then harden KVStore delete correctness, WAL durability behavior, and Raft restart persistence in four reviewable milestones.

**Architecture:** The plan keeps the current repository shape and upgrades it incrementally: first make the WSL worktree build path reliable, then add tombstone-aware storage semantics, then bound and strengthen WAL durability behavior, and finally persist the minimum durable Raft state required for restart. Each milestone adds tests first, lands minimal implementation, and commits independently.

**Tech Stack:** C++20, CMake, WSL bash scripts, gRPC/protobuf bootstrap scripts, deterministic C++ tests, git worktrees

---

## File Map

- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\bootstrap_grpc_runtime.sh`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\bootstrap_proto_tools.sh`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\check_proto_contract_compile.sh`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\local_check.sh`
- Create or modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\.gitattributes`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\memtable.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\memtable.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\wal.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\wal.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\sstable.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\sstable.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\kv_engine.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\compaction.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\compaction.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_storage.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_storage.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_node.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_node.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\cluster_runtime.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\CMakeLists.txt`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\wal_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\recovery_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\sstable_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\compaction_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_replication_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_failover_test.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_persistence_test.cpp`

### Task 1: Repair WSL Worktree Script Baseline

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\bootstrap_grpc_runtime.sh`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\bootstrap_proto_tools.sh`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\check_proto_contract_compile.sh`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\scripts\ci\local_check.sh`
- Create or modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\.gitattributes`
- Test: WSL configure/build commands from the repository root

- [ ] **Step 1: Add a failing baseline check that proves shell scripts currently have CRLF-sensitive execution under WSL**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && bash scripts/ci/bootstrap_grpc_runtime.sh"
```

Expected: FAIL with `set: pipefail\r: invalid option name` or equivalent CRLF-related shell parsing error.

- [ ] **Step 2: Add or extend `.gitattributes` so repository shell scripts are normalized to LF**

```gitattributes
*.sh text eol=lf
scripts/** text eol=lf
```

If `.gitattributes` already exists, add only the minimal LF rules needed for shell scripts.

- [ ] **Step 3: Re-save the affected shell scripts with LF line endings and no behavior change**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && perl -pi -e 's/\r$//' scripts/ci/bootstrap_grpc_runtime.sh scripts/ci/bootstrap_proto_tools.sh scripts/ci/check_proto_contract_compile.sh scripts/ci/local_check.sh"
```

- [ ] **Step 4: Run the failing baseline check again to verify the CRLF blocker is gone**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && bash scripts/ci/bootstrap_grpc_runtime.sh"
```

Expected: no `pipefail\r` parsing error. It may still perform bootstrap work; the CRLF failure must be gone.

- [ ] **Step 5: Configure the project from WSL using a clean build directory**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && rm -rf build && cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug"
```

Expected: configure succeeds.

- [ ] **Step 6: Build the project from WSL**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening' && cmake --build build -j2"
```

Expected: build succeeds.

- [ ] **Step 7: Run a representative baseline slice**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(smoke|wal|raft_failover)_test'"
```

Expected: selected tests pass.

- [ ] **Step 8: Commit**

```bash
git add .gitattributes scripts/ci/bootstrap_grpc_runtime.sh scripts/ci/bootstrap_proto_tools.sh scripts/ci/check_proto_contract_compile.sh scripts/ci/local_check.sh
git commit -m "chore(build): fix wsl worktree bootstrap baseline"
```

### Task 2: Add a Tombstone-Aware MemTable Model

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\memtable.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\memtable.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\kv_engine.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\recovery_test.cpp`

- [ ] **Step 1: Write a failing recovery test for delete-after-flush semantics**

Add a new scenario in `tests/recovery_test.cpp` that:

```cpp
// Pseudocode shape to implement literally in the test:
// 1. Open engine
// 2. Put("k1", "v1", "req-1")
// 3. Flush()
// 4. Delete("k1", "req-2")
// 5. Destroy engine
// 6. Reopen engine
// 7. Assert Get("k1") is std::nullopt
```

- [ ] **Step 2: Run the targeted test to verify it fails**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_recovery_test"
```

Expected: FAIL because current delete semantics do not preserve a tombstone across flush/reopen.

- [ ] **Step 3: Introduce a tombstone-capable memtable entry model**

Implement a value model in `memtable.h/.cpp` equivalent to:

```cpp
struct ValueEntry {
  bool tombstone = false;
  std::string value;
};
```

and change storage behavior so:

```cpp
// Put
kv_[mutation.key] = ValueEntry{.tombstone = false, .value = mutation.value};

// Delete
kv_[mutation.key] = ValueEntry{.tombstone = true, .value = ""};
```

Expose a read API that distinguishes:

```cpp
enum class LookupState { kMissing, kValue, kTombstone };
```

or an equivalent optional struct carrying tombstone state.

- [ ] **Step 4: Update `KvEngine::Get` to stop on tombstones**

Implement the engine-side logic so:

```cpp
const auto mem_value = memtable_.Get(key);
if (mem_value.state == LookupState::kValue) {
  return mem_value.value;
}
if (mem_value.state == LookupState::kTombstone) {
  return std::nullopt;
}
```

Do not fall through to older SSTs once a tombstone is found.

- [ ] **Step 5: Run the targeted recovery test again**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_recovery_test"
```

Expected: still FAIL, but now for persistence gaps rather than in-memory delete handling.

### Task 3: Persist Tombstones in WAL and Recovery

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\wal.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\wal.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\wal_test.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\recovery_test.cpp`

- [ ] **Step 1: Add a failing WAL/recovery test that requires delete replay to preserve tombstones**

Extend `tests/wal_test.cpp` or `tests/recovery_test.cpp` with a case that:

```cpp
// 1. Put a key
// 2. Delete the key
// 3. Reopen the engine
// 4. Verify the key is absent
// 5. Verify delete replay does not regress to old value visibility
```

- [ ] **Step 2: Run the targeted tests to verify they fail**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(wal|recovery)_test'"
```

Expected: FAIL because replay restores deletes only by erase, not by persisted tombstone semantics.

- [ ] **Step 3: Keep WAL operation tags explicit and map delete replay into tombstone state**

Make sure WAL replay reconstructs:

```cpp
mutation.type = (record.operation == WalOperation::kPut)
                    ? MutationType::kPut
                    : MutationType::kDelete;
```

and that memtable apply for delete produces a tombstone entry rather than erasing the key from the in-memory state.

- [ ] **Step 4: Preserve request-id idempotency while replaying tombstones**

Keep this invariant:

```cpp
if (applied_request_ids_.contains(mutation.request_id)) {
  return ApplyDisposition::kDuplicate;
}
```

and ensure delete replay still inserts the request id before storing the tombstone entry.

- [ ] **Step 5: Run the WAL and recovery tests again**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(wal|recovery)_test'"
```

Expected: PASS for the new replay behavior.

### Task 4: Add Tombstones to SST Encoding and Read Semantics

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\sstable.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\sstable.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\sstable_test.cpp`

- [ ] **Step 1: Write a failing SST test for persisted tombstones**

Add a new case in `tests/sstable_test.cpp` that encodes data equivalent to:

```cpp
// entries:
// ("k1", "v1", not tombstone)
// ("k2", "", tombstone)
//
// open reader
// assert Get("k1") => found value
// assert Get("k2") => explicit tombstone / not found without reading older state
```

- [ ] **Step 2: Run the SST test to verify it fails**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_sstable_test"
```

Expected: FAIL because SST entries currently encode only key/value pairs.

- [ ] **Step 3: Extend SST entry encoding with an explicit entry kind**

Add a compact per-entry tag in the block payload, for example:

```cpp
enum class SstEntryKind : std::uint8_t {
  kValue = 1,
  kTombstone = 2,
};
```

and encode each entry as:

```cpp
[u8 kind][u32 key_size][u32 value_size][key bytes][value bytes]
```

For tombstones, `value_size` must be `0`.

- [ ] **Step 4: Update parse/read paths to surface tombstone results explicitly**

Implement reader-side behavior so a found tombstone becomes a dedicated result state, not a generic miss.

Equivalent shape:

```cpp
struct SstGetResult {
  bool found = false;
  bool tombstone = false;
  std::optional<std::string> value;
  std::optional<integrity::IntegrityError> error;
};
```

- [ ] **Step 5: Update `KvEngine::Flush` to serialize tombstones from the memtable**

Change `SortedEntries()` or an equivalent API so flush receives key plus entry metadata, not only key/value.

- [ ] **Step 6: Run the SST test again**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_sstable_test"
```

Expected: PASS.

### Task 5: Make Compaction Preserve Tombstones

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\compaction.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\compaction.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\compaction_test.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\recovery_test.cpp`

- [ ] **Step 1: Write a failing compaction test for tombstone masking**

Add a new case in `tests/compaction_test.cpp`:

```cpp
// 1. Put("k1", "v1"), Flush()
// 2. Delete("k1"), Flush()
// 3. Compact()
// 4. Assert Get("k1") is std::nullopt
```

- [ ] **Step 2: Run the compaction test to verify it fails**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_compaction_test"
```

Expected: FAIL because current compaction merges only key/value visibility, not tombstone semantics.

- [ ] **Step 3: Update compaction merge logic to treat tombstones as first-class newest-wins entries**

Implement merge behavior so the newest entry for a key wins whether it is a value or a tombstone.
Do not drop tombstones during compaction in this pass.

Equivalent merge rule:

```cpp
// newest to oldest inputs
if (key not yet emitted) {
  emit newest entry, including tombstone entries;
}
```

- [ ] **Step 4: Run compaction and recovery tests again**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(compaction|recovery)_test'"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/kvstore/engine/memtable.h src/engine/memtable.cpp include/kvstore/engine/wal.h src/engine/wal.cpp include/kvstore/engine/sstable.h src/engine/sstable.cpp include/kvstore/engine/kv_engine.h src/engine/kv_engine.cpp include/kvstore/engine/compaction.h src/engine/compaction.cpp tests/wal_test.cpp tests/recovery_test.cpp tests/sstable_test.cpp tests/compaction_test.cpp
git commit -m "fix(engine): persist tombstones across flush and compaction"
```

### Task 6: Add WAL Generations and Ordered Replay

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\wal.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\wal.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\kv_engine.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\wal_test.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\recovery_test.cpp`

- [ ] **Step 1: Write a failing test for replay across multiple WAL generations**

Add a test that creates a directory containing:

```cpp
000001.wal
000002.wal
```

with ordered mutations across both files, then reopens the engine and asserts final state reflects replay of both generations in ascending order.

- [ ] **Step 2: Run the WAL/recovery tests to verify they fail**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(wal|recovery)_test'"
```

Expected: FAIL because current engine opens only one fixed WAL path.

- [ ] **Step 3: Introduce WAL generation discovery and ordered replay**

Implement helpers in `wal.h/.cpp` and `kv_engine.cpp` with behavior equivalent to:

```cpp
std::vector<std::filesystem::path> DiscoverWalSegments(const std::filesystem::path& dir);
```

Rules:

```cpp
// include only *.wal files with numeric stems
// sort by numeric generation ascending
// replay all generations before opening the active writer
```

- [ ] **Step 4: Rotate to a new active WAL after flush**

Implement a minimal rollover policy:

```cpp
// after successful Flush():
// 1. clear memtable key state only after SST is durable enough for current repo semantics
// 2. increment wal generation
// 3. open a fresh active WAL for future writes
```

Keep old generations available for replay until they are proven safely superseded.

- [ ] **Step 5: Re-run WAL/recovery tests**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(wal|recovery)_test'"
```

Expected: PASS.

### Task 7: Strengthen WAL Sync Semantics

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\wal.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\wal.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\wal_test.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\integration\bench_gate_test.cpp`

- [ ] **Step 1: Write a failing unit test for explicit sync on append**

Add a focused test in `tests/wal_test.cpp` that exercises append and then asserts the writer reports success only when the underlying stream/file descriptor sync step succeeds.
If direct injection is needed, add a minimal test seam around the sync call rather than mocking the whole writer.

- [ ] **Step 2: Run the WAL test to verify it fails**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_wal_test"
```

Expected: FAIL because append currently flushes but does not perform an explicit stronger sync step.

- [ ] **Step 3: Add a platform-contained sync step inside `WalWriter::Append`**

Implement a small internal helper with behavior equivalent to:

```cpp
bool SyncFile(std::FILE* file);
```

or, if the writer is stream-backed:

```cpp
// flush stream first
// then obtain file descriptor and call fsync on POSIX
```

Keep this logic isolated inside WAL internals.

- [ ] **Step 4: Re-run WAL and benchmark gate tests**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(wal|bench_gate)_test'"
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/kvstore/engine/wal.h src/engine/wal.cpp include/kvstore/engine/kv_engine.h src/engine/kv_engine.cpp tests/wal_test.cpp tests/recovery_test.cpp tests/integration/bench_gate_test.cpp
git commit -m "feat(engine): add wal rotation and stronger sync semantics"
```

### Task 8: Add Durable Raft Storage Primitives

**Files:**
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_storage.h`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_storage.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\CMakeLists.txt`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_persistence_test.cpp`

- [ ] **Step 1: Write a failing Raft persistence test for term, vote, and log reload**

Create `tests/raft_persistence_test.cpp` with a direct storage-level test shaped like:

```cpp
// 1. Create storage rooted in temp dir
// 2. Persist current_term=3, voted_for=2
// 3. Append two log entries
// 4. Destroy storage
// 5. Reopen storage
// 6. Assert term, vote, and entries survive
```

- [ ] **Step 2: Run the new test to verify it fails**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_raft_persistence_test"
```

Expected: FAIL because storage component does not exist yet.

- [ ] **Step 3: Implement a minimal local-disk Raft storage component**

Create `raft_storage.h/.cpp` with an API equivalent to:

```cpp
struct PersistentRaftState {
  Term current_term = 0;
  NodeId voted_for = kNoVote;
  std::vector<LogEntry> log;
};

class RaftStorage {
 public:
  explicit RaftStorage(std::filesystem::path dir);
  bool Load(PersistentRaftState* out);
  bool StoreMetadata(Term term, NodeId voted_for);
  bool StoreLog(const std::vector<LogEntry>& log);
};
```

Use a simple repository-appropriate file format in this pass; correctness and recoverability matter more than format sophistication.

- [ ] **Step 4: Re-run the persistence test**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R kvstore_raft_persistence_test"
```

Expected: PASS.

### Task 9: Integrate Durable Metadata and Log into `RaftNode`

**Files:**
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_node.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_node.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\cluster_runtime.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\kv_raft_service.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_replication_test.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_failover_test.cpp`
- Test: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_persistence_test.cpp`

- [ ] **Step 1: Write a failing node-level restart test**

Extend `tests/raft_persistence_test.cpp` with a node-oriented scenario:

```cpp
// 1. Start a node with durable storage
// 2. Force election or set state transitions that persist term/vote
// 3. Propose at least one log entry
// 4. Recreate the node from the same storage path
// 5. Assert loaded term/vote/log match the persisted values
```

- [ ] **Step 2: Run the persistence and replication tests to verify the node-level behavior fails**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(raft_persistence|raft_replication|raft_failover)_test'"
```

Expected: FAIL because `RaftNode` does not yet load or store durable protocol state.

- [ ] **Step 3: Load persisted metadata/log on node construction**

Integrate storage during initialization with behavior equivalent to:

```cpp
if (storage_) {
  PersistentRaftState persisted;
  if (storage_->Load(&persisted)) {
    current_term_ = persisted.current_term;
    voted_for_ = persisted.voted_for;
    log_ = persisted.log.empty() ? std::vector<LogEntry>{LogEntry{.term = 0, .command = ""}} : persisted.log;
  }
}
```

- [ ] **Step 4: Persist term/vote transitions at protocol boundaries**

Store metadata on changes equivalent to:

```cpp
// BecomeFollower on higher term
// StartElection after incrementing term and voting for self
// vote grant when voted_for_ changes
```

- [ ] **Step 5: Persist log changes when leader appends**

After appending a new entry or leader no-op:

```cpp
log_.push_back(...);
PersistLogOrFailClosed();
```

Keep the change minimal: if persistence fails, do not pretend the append succeeded.

- [ ] **Step 6: Re-run persistence, replication, and failover tests**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(raft_persistence|raft_replication|raft_failover)_test'"
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add include/kvstore/raft/raft_storage.h src/raft/raft_storage.cpp include/kvstore/raft/raft_node.h src/raft/raft_node.cpp src/raft/cluster_runtime.cpp src/service/kv_raft_service.cpp src/CMakeLists.txt tests/raft_replication_test.cpp tests/raft_failover_test.cpp tests/raft_persistence_test.cpp
git commit -m "feat(raft): persist metadata and log state"
```

### Task 10: Full Verification Pass

**Files:**
- Verify only

- [ ] **Step 1: Run the core engine and Raft regression suite**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(smoke|wal|recovery|sstable|compaction|raft_election|raft_replication|raft_quorum|raft_failover|raft_persistence)_test'"
```

Expected: PASS.

- [ ] **Step 2: Run the gRPC and integration slice most likely to catch regressions**

```bash
wsl /bin/bash -lc "cd '/mnt/c/学校/KVStore/.worktrees/kvstore-hardening/build' && ctest --output-on-failure -R 'kvstore_(grpc_integration|grpc_idempotency|api_status|bench_gate|chaos_gate)_test'"
```

Expected: PASS.

- [ ] **Step 3: Inspect git status**

```bash
git status --short
```

Expected: clean working tree.

- [ ] **Step 4: If docs need updates for new durability or delete semantics, add them and commit separately only if the verification run demonstrates they are necessary**

```bash
# Only if needed:
git add docs/...
git commit -m "docs: update storage and raft persistence behavior"
```

## Plan Review Notes

- Spec coverage is complete across all four milestones: WSL baseline, tombstones, WAL durability lifecycle, and durable Raft state.
- The plan intentionally keeps snapshotting and dynamic membership out of scope.
- Inline execution is the default for this session because the user explicitly requested continuing without stopping for confirmation.
