# Interview Q&A V2 Design

## Goal

Replace the existing KVStore interview Q&A with a current-state version that reflects the hardened repository rather than the earlier embedded-only prototype.

## Why Rewrite

The old interview Q&A is now stale in several important ways:

- it still defines the project primarily as a single-process embedded five-node system
- it still describes tombstones, WAL rotation, stronger sync semantics, durable Raft metadata/log, and peer networking as future work
- it does not cover the new `cluster-node` runtime, same-host multi-process cluster behavior, snapshot support, or `InstallSnapshot` catch-up

If left unchanged, the file would train the user to undersell or misdescribe the current project state in interviews.

## Scope

The rewrite will:

- overwrite the existing interview Q&A file in place
- keep the target audience the same: C++ / infrastructure interviews
- center the current project state on the `kvstore-hardening` branch
- preserve useful explanatory structure while removing stale claims

The rewrite will not:

- preserve the previous question numbering for compatibility
- maintain a historical "v1 embedded-only" framing as the main story
- try to become a complete tutorial on distributed systems

## Recommended Structure

The new document should be organized around the project as it exists now:

1. Current project positioning
2. Architecture and runtime modes
3. Storage engine and durability
4. Raft, snapshots, and consistency semantics
5. Multi-process cluster-node runtime
6. gRPC API and service-layer behavior
7. Testing, verification, and evidence
8. High-value tricky follow-ups

## Content Principles

- State current reality first.
- Mention earlier limitations only as historical evolution, not current gaps.
- Be explicit about what is still not production-grade.
- Prefer answers that survive technical follow-up.
- Add dedicated coverage for:
  - `embedded` vs `cluster-node`
  - peer RPC and transport abstraction
  - WAL rotation and stronger sync semantics
  - durable Raft metadata/log
  - snapshots and `InstallSnapshot`
  - same-host multi-process failover testing

## Deliverable

One rewritten markdown file:

- `docs/interview/cpp_infra_kvstore_interview_qa.md`

It should read like a current, interview-ready question bank for the code that now exists in the repository.
