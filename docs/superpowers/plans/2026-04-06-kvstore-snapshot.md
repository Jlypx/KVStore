# KVStore Snapshot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add local snapshot creation, Raft log truncation, durable snapshot metadata, and `InstallSnapshot` catch-up to the current KVStore implementation.

**Architecture:** The implementation keeps snapshot creation coordinated in the service layer while extending Raft storage and indexing with snapshot base metadata. Snapshot transfer is added as a unary peer RPC, and the engine gains full-state export/import so a follower can install a replacement state machine snapshot before truncating its Raft log.

**Tech Stack:** C++20, current Raft persistence layer, current engine WAL/SST layer, gRPC/protobuf peer transport, embedded deterministic tests, same-host cluster runtime

---

## File Map

- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_types.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\proto\kvstore\v1\raft.proto`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_proto_conversion.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_proto_conversion.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_storage.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_storage.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\raft_node.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\raft_node.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\memtable.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\memtable.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\engine\kv_engine.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\engine\kv_engine.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\service\kv_raft_service.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\kv_raft_service.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\service\cluster_node_service.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\cluster_node_service.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\service\raft_peer_service.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\service\raft_peer_service.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\include\kvstore\raft\network_transport.h`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\raft\network_transport.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\src\CMakeLists.txt`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_persistence_test.cpp`
- Create: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\snapshot_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\raft_replication_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\integration\multi_process_cluster_test.cpp`
- Modify: `C:\学校\KVStore\.worktrees\kvstore-hardening\tests\CMakeLists.txt`

## Plan Review Notes

- This plan intentionally uses deterministic embedded coverage first; multi-process snapshot catch-up is optional after the embedded path passes.
- The user explicitly asked to implement immediately, so execution will continue inline after this plan is written.
