# KVStore v1 Scope Contract

## Purpose

This document freezes the v1 architecture and API boundaries for the Wave 1 Task 2 contract gate. It is intentionally strict to prevent scope creep before storage and consensus implementation begins.

## v1 API (Frozen)

- Service: `kvstore.v1.KV`
- RPCs: unary `Put`, unary `Get`, unary `Delete`
- Explicitly excluded in v1: `Txn`, `Watch`, `Lease`, `RangeScan`, streaming RPCs

## Input Limits and Defaults

- Key size limit: `<= 1 KiB`
- Value size limit: `<= 1 MiB`
- Membership default: static `5` voting nodes
- Read consistency default: linearizable

## Architecture Boundaries

The implementation must preserve the following module boundaries:

- `api`: protobuf contracts and gRPC handlers only; no storage internals
- `service`: request validation, idempotency, and orchestration
- `engine`: core KV state machine and storage abstractions
- `cache`: hot-data acceleration and eviction policy
- `integrity`: checksums, corruption detection, and integrity error taxonomy
- `observability`: metrics, structured logs, and health/readiness signals

## Deferred Scope (Explicit)

The following items are intentionally deferred beyond v1:

- Multi-operation transactions (`Txn`)
- Dynamic membership change (add/remove/reconfigure node)
- Watch/lease APIs and long-lived subscriptions
- Range scan and secondary index capabilities
- Multi-region replication

## Contract Guard

`scripts/ci/check_scope_contract.sh` is the CI gate for this scope contract and must fail on any out-of-scope v1 proto RPC additions.
