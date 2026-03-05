# ADR 0004: Deferred Scope Boundary for v1

- Status: Accepted
- Date: 2026-03-05

## Context

Task 2 must explicitly freeze deferred scope to avoid architecture and API expansion during foundational implementation waves.

## Decision Drivers

- Keep Task 3-6 deliverables executable without feature creep
- Preserve deterministic acceptance gates
- Prevent incomplete features from weakening correctness guarantees

## Considered Options

1. Strictly defer non-core APIs and topology changes from v1
2. Add partial Txn/Watch/Lease support in v1
3. Keep scope intentionally open and decide per implementation phase

## Decision

Adopt option 1 for v1. The following are out of scope:

- `Txn` and multi-operation atomic API surface
- `Watch` and `Lease` APIs
- `RangeScan` and secondary-index capabilities
- Dynamic membership (add/remove/reconfigure nodes)
- Multi-region replication

## Rationale

Deferral keeps the v1 contract auditable and aligned with correctness-first milestones. Each deferred feature introduces independent semantics and failure modes that are unsafe to add before core durability and consensus are stable.

## Consequences

- Positive:
  - Limits complexity for early implementation and testing
  - Makes CI contract checks straightforward and enforceable
- Negative:
  - Reduced feature completeness for early adopters
  - Some workflows require client-side orchestration until later milestones

## Confirmation Strategy

- Run `scripts/ci/check_scope_contract.sh` to fail on forbidden v1 RPC additions
- Keep exclusions listed in `docs/architecture/scope.md`
- Record violations in CI evidence under `.sisyphus/evidence/`
