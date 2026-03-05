# ADR 0001: Linearizable Consistency as v1 Default

- Status: Accepted
- Date: 2026-03-05

## Context

Task 2 requires architecture contracts that keep v1 scope small but correctness-focused. The plan fixes read semantics to linearizable behavior in v1 and positions this as a blocker decision for downstream engine/Raft/API work.

## Decision Drivers

- Prevent stale-read ambiguity in early distributed releases
- Align with enterprise correctness expectations for acknowledged writes
- Keep v1 behavior deterministic for automated failover testing

## Considered Options

1. Linearizable reads by default for all `Get` requests
2. Serializable/stale reads by default with optional linearizable mode
3. Mixed mode from day one with client-selectable read consistency

## Decision

Adopt option 1 for v1: `Get` is linearizable by default and the primary documented behavior.

## Rationale

Linearizable-by-default reduces correctness surprises and removes semantic branching in early implementation. This preserves a smaller, auditable contract while consensus and storage paths stabilize.

## Consequences

- Positive:
  - Simplifies API expectations for clients and tests
  - Directly supports no-acknowledged-write-loss goals
- Negative:
  - May trade read latency for stronger consistency
  - Requires careful leader/read-index integration in later tasks
- Deferred:
  - Serializable/stale-read mode can be added post-v1 with explicit API versioning

## Confirmation Strategy

- Enforce v1 unary-only API in `proto/kvstore/v1/kv.proto`
- Keep linearizable default documented in `docs/architecture/scope.md`
- Validate with future failover/read freshness integration checks in Task 6/7 evidence logs
