# KVStore Hardening Design

## Purpose

This design defines a four-milestone hardening pass for the current KVStore v1 repository.
The goal is to fix real correctness and durability gaps in the current implementation without pretending the system is already a production database.

The work is intentionally staged:

1. Restore a reliable WSL-based worktree build/test baseline.
2. Fix delete semantics by introducing persisted tombstones.
3. Tighten durability and WAL lifecycle behavior.
4. Persist Raft metadata and log state so restart behavior is closer to durable Raft semantics.

Each milestone is expected to land as its own atomic commit with tests.

## Current State

The current repository already has a coherent architecture:

- `api` exposes unary gRPC `Put/Get/Delete`
- `service` validates, enforces idempotency, and coordinates Raft + engine
- `raft` handles leader election, replication, and commit advancement
- `engine` handles WAL, memtable, SSTables, compaction, and block cache

However, there are four meaningful gaps:

### 1. Worktree / WSL build baseline is fragile

The isolated worktree currently cannot configure cleanly in this environment.
The latest evidence shows that running CMake inside WSL reaches the repo-local gRPC bootstrap scripts, but `bash` fails on CRLF line endings in shell scripts.
This is a prerequisite issue because implementation and verification both depend on a stable isolated build.

### 2. Delete semantics are incomplete across flushed SSTs

`Delete` currently erases the key from the memtable but does not persist a tombstone record into SSTables.
That means a key deleted after an older flush can reappear when reads fall through to an older SST.
This is a correctness bug, not just an optimization issue.

### 3. Durability behavior is weaker than the current API story suggests

The engine appends and flushes WAL data, but the current implementation does not clearly separate stream flush from stronger sync semantics, and WAL lifecycle management is minimal.
The repo also keeps replaying a single WAL forever, which grows recovery cost and muddies durability boundaries.

### 4. Raft state is not durably persisted

The current implementation persists state-machine data through `KvEngine`, but not Raft term/vote/log state.
After restart, the system rebuilds an in-memory Raft cluster and relies on recovered state-machine data.
That is useful for the current prototype but falls short of durable Raft behavior.

## Goals

- Make the isolated worktree build/test path reliable under the current WSL-driven workflow.
- Make deletes semantically correct across memtable, WAL replay, SST reads, and compaction.
- Strengthen durability semantics so acknowledged writes have a clearer persistence boundary and recovery cost is bounded by a more disciplined WAL lifecycle.
- Persist the minimum Raft state required for durable restart semantics: `current_term`, `voted_for`, and the Raft log.
- Keep the external API surface unchanged: still only `Put/Get/Delete`.
- Land the work incrementally with tests and one commit per milestone.

## Non-Goals

- No dynamic membership.
- No snapshot/install-snapshot protocol in this pass.
- No networked multi-process Raft transport.
- No change to the public protobuf API.
- No attempt to turn the repository into a fully productionized database in one pass.

## Approaches Considered

### Approach A: Fix only the three core storage/consensus gaps

This would skip the worktree/WSL baseline and go directly to tombstones, durability, and Raft persistence.

Pros:

- Directly targets the user-visible system gaps.

Cons:

- Not practical in the current environment because clean isolated verification is blocked.
- Risks mixing environment breakage with feature regressions.

### Approach B: Do a large all-at-once refactor

This would redesign storage formats, WAL lifecycle, and Raft persistence together.

Pros:

- Could produce a cleaner end state on paper.

Cons:

- High risk.
- Hard to review.
- Hard to test incrementally.
- Conflicts with the requirement to commit frequently.

### Approach C: Recommended staged hardening

This design uses four milestones, each narrowly scoped and independently testable.

Pros:

- Matches the actual repository structure.
- Keeps failures local and reviewable.
- Supports frequent commits.
- Preserves momentum while still fixing deep issues.

Cons:

- Requires more total steps and more disciplined sequencing.

## Recommended Design

### Milestone 0: WSL / Worktree baseline repair

#### Intent

Make the isolated worktree configure, build, and test reliably from WSL so later milestones can be developed and verified in the intended environment.

#### Design

- Normalize repo shell/bootstrap scripts that are executed by WSL bash to LF line endings.
- If needed, add repo-local safeguards so required shell scripts are treated as text with stable line endings.
- Rebuild the worktree from WSL-native CMake rather than mixing Windows-generated build trees with WSL-generated ones.
- Verify that the repo-local gRPC/protobuf bootstrap path still works in the isolated tree.

#### Success Criteria

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` succeeds in WSL in the worktree.
- `cmake --build build` succeeds in WSL in the worktree.
- A representative baseline test slice runs successfully before feature work begins.

### Milestone 1: Tombstone-backed delete correctness

#### Intent

Make deletes durable and semantically correct across flush, compaction, and recovery.

#### Design

- Extend the in-memory mutation/value model so a key can exist in one of three states:
  - present with value
  - deleted tombstone
  - absent / unknown
- Persist delete intent into WAL replay and SST files instead of dropping it at flush time.
- Teach reads to stop on a tombstone rather than continuing into older SSTs.
- Teach compaction to merge tombstones correctly so old values remain masked.

#### Likely Structural Changes

- `MemTable` will need to retain tombstone state instead of deleting keys outright.
- SST block encoding will need to carry a per-entry tombstone marker or equivalent operation tag.
- Compaction merge logic must preserve newest-wins semantics for both values and tombstones.
- Recovery tests must verify that deleted keys stay deleted after restart and after flush/compaction.

#### Success Criteria

- A key written, flushed, deleted, flushed, and read back must remain deleted.
- A deleted key must not reappear from older SST files.
- Compaction must preserve deletion semantics.

### Milestone 2: Durability and WAL lifecycle tightening

#### Intent

Move the engine closer to an honest durable-write story and keep recovery time from depending on an endlessly growing single WAL.

#### Design

- Introduce a stronger WAL sync step after append, using platform-appropriate file-descriptor sync on the underlying file.
- Ensure the write acknowledgment path still corresponds to the mutation having reached the intended persistence boundary.
- Add WAL rotation / generation handling tied to flush or checkpoint boundaries so replay can be bounded.
- On open, replay WAL segments in order and continue writing to the active generation.

#### Boundaries

- This pass does not need to implement full manifest/version-set machinery.
- The design should stay minimal and repository-appropriate: generation naming, ordered replay, retirement of safely superseded WALs.

#### Success Criteria

- Recovery still works across restarts.
- WAL replay uses ordered generations rather than a forever-growing single file.
- Tests demonstrate that acknowledged writes still survive the intended crash model.

### Milestone 3: Durable Raft metadata and log

#### Intent

Persist the minimum Raft state needed so node restarts preserve protocol state rather than rebuilding everything from scratch.

#### Design

- Add a small Raft storage component responsible for persisting:
  - `current_term`
  - `voted_for`
  - log entries
- Load this state at node startup before the node begins ticking or participating in elections.
- Update Raft transitions so term/vote changes are persisted before they are exposed.
- Persist proposed log entries before they are considered part of the node log.
- On restart, rebuild in-memory Raft state from durable storage and then resume normal election/replication behavior.

#### Boundaries

- This pass focuses on durable local-node restart behavior.
- Snapshotting can remain future work as long as the log persistence contract is explicit.

#### Success Criteria

- A restarted node preserves its prior Raft term/vote state.
- Persisted log entries survive restart.
- Existing failover/replication semantics continue to hold.

## Data Flow Impact

### Write Path After Milestones 1-3

1. Client request reaches `KvRaftService`.
2. Service validates request and deduplicates by `request_id`.
3. Leader proposes a Raft command.
4. Leader persists the Raft log entry before treating it as durable local protocol state.
5. Majority replication leads to commit.
6. State-machine apply writes the mutation to the engine WAL with durable sync semantics.
7. Memtable records either value or tombstone state.
8. Flush writes values and tombstones into SSTables.
9. Compaction merges entries while respecting newest-wins and tombstones.
10. Service returns success only after the committed result is available.

### Read Path After Milestone 1

1. Service requires leader ownership and quorum contact.
2. Engine checks memtable first.
3. If a tombstone is found, read stops and returns not found.
4. If not found in memtable, search SSTables newest to oldest.
5. If a tombstone is found in any SST, read stops and returns not found.
6. Older SSTs are not consulted past a tombstone boundary.

## Testing Strategy

### Milestone 0

- WSL worktree configure/build verification.
- Representative baseline tests, likely:
  - `kvstore_smoke_test`
  - one engine test
  - one Raft test

### Milestone 1

- Add failing tests first for:
  - delete after flush should mask old SST value
  - delete should survive restart
  - compaction should preserve delete semantics

### Milestone 2

- Add failing tests first for:
  - replay across multiple WAL generations
  - active WAL rollover after flush/checkpoint boundary
  - acknowledged write still survives restart / leader crash under the intended durability model

### Milestone 3

- Add failing tests first for:
  - restart preserves term/vote
  - restart preserves log entries
  - restarted node rejoins without violating existing replication/failover behavior

## Commit Strategy

The work should land as separate commits in this order:

1. `chore(build): fix wsl worktree bootstrap baseline`
2. `fix(engine): persist tombstones across flush and compaction`
3. `feat(engine): add wal rotation and stronger sync semantics`
4. `feat(raft): persist metadata and log state`

Documentation updates, if needed, should either be folded into the milestone commit they describe or added as a short follow-up commit only when necessary.

## Risks and Mitigations

### Risk: Tombstone support bleeds into too many layers

Mitigation:

- Keep the representation explicit.
- Add tests before changing file formats.
- Prefer a single “entry kind” concept shared by memtable, WAL, and SST encoding.

### Risk: Durability changes become OS-specific and brittle

Mitigation:

- Use minimal, well-contained platform handling.
- Keep stronger sync logic local to WAL writer internals.
- Verify behavior through tests, not only by inspection.

### Risk: Raft persistence changes protocol timing subtly

Mitigation:

- Persist state at clear protocol boundaries.
- Extend deterministic tests before broadening integration tests.
- Change one Raft storage concern at a time: metadata first, then log semantics if needed.

### Risk: Scope explosion

Mitigation:

- No snapshots in this pass.
- No dynamic membership.
- No public API expansion.
- One milestone, one commit, one verification slice at a time.

## Deliverable

At the end of this hardening pass, the repository should still look like the same KVStore v1 project, but with:

- a reliable WSL worktree development path,
- correct tombstone-backed delete behavior,
- more credible WAL durability/recovery behavior,
- and persisted Raft protocol state across restart.
