# ADR 0002: Static Five-Node Membership in v1

- Status: Accepted
- Date: 2026-03-05

## Context

The plan sets cluster fault tolerance to 5 nodes with tolerance for up to 2 failures and explicitly defers dynamic membership for v1.

## Decision Drivers

- Reduce consensus complexity in first production-hardening cycle
- Preserve predictable quorum behavior for deterministic tests
- Avoid unsafe partial implementation of membership reconfiguration

## Considered Options

1. Static membership of 5 voting nodes in v1
2. Dynamic add/remove membership in v1
3. Hybrid approach (static bootstrap plus limited runtime changes)

## Decision

Adopt option 1 for v1: static 5-voter membership only; no runtime add/remove/reconfigure operations.

## Rationale

Dynamic membership introduces non-trivial safety and operational complexity. Freezing topology in v1 allows focus on correctness of election, replication, and durability before widening control-plane scope.

## Consequences

- Positive:
  - Smaller state-transition surface for Raft correctness
  - Easier reproducible chaos tests and failure analysis
- Negative:
  - No elasticity for node replacement/scale-out in v1
  - Operational workflows require full-cluster coordinated changes
- Deferred:
  - Dynamic membership APIs and workflows move to post-v1 milestone

## Confirmation Strategy

- Keep static membership default documented in `docs/architecture/scope.md`
- Ensure no membership mutation RPCs appear in v1 proto or service docs
- Validate static 5-node behavior in future Task 5/7 cluster evidence
