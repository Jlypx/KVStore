# ADR 0003: End-to-End Checksum Verification for v1 Durability Path

- Status: Accepted
- Date: 2026-03-05

## Context

Task 2 requires an integrity/checksum decision that constrains downstream WAL/SST implementation and corruption-handling behavior.

## Decision Drivers

- Detect on-disk corruption deterministically
- Prevent silent data acceptance during replay/read
- Provide stable integrity error signals for observability gates

## Considered Options

1. Checksum WAL records and SST blocks, verify on replay/read
2. Checksum WAL only, trust SST layers
3. Rely on storage/hardware checks only (no application-level verification)

## Decision

Adopt option 1 for v1: checksum verification is mandatory for WAL records and SST blocks on read/replay paths.

## Rationale

Integrity must be an explicit application contract, not an assumed property of underlying storage. Verifying both transient and persisted structures gives deterministic corruption detection and clear failure semantics.

## Consequences

- Positive:
  - Corruption becomes detectable and testable through automation
  - Integrity error taxonomy can be exposed consistently across layers
- Negative:
  - Additional CPU overhead on write/read/replay paths
  - More implementation complexity in engine and integrity modules
- Deferred:
  - Advanced checksum algorithm tuning and optional hardware offload

## Confirmation Strategy

- Keep `integrity` boundary explicit in `docs/architecture/scope.md`
- Add deterministic corruption-injection tests in later tasks with evidence logs
- Require integrity detection counters/logs in observability gates
