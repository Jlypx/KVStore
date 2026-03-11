# KVStore v1 Security Posture

## Purpose

This document describes the current implemented v1 security posture.
It covers the security properties the repository provides today, the trust boundaries those properties apply to, and the explicit gaps that remain out of scope.
It is evidence-backed and limited to behavior implemented in `src/bin/kvd.cpp`, `src/service/kv_raft_service.cpp`, `src/engine/wal.cpp`, `src/engine/sstable.cpp`, the v1 ADR set, and Task 7 validation artifacts.

## Threat model and trust boundaries for v1

The current implementation protects a narrow set of boundaries.

- Client to server transport boundary: one client-facing gRPC listener per `kvd` process, implemented in `src/bin/kvd.cpp`
- Service validation boundary: request sizes and request identity checks enforced in `src/service/kv_raft_service.cpp` before a mutation is proposed into Raft
- Storage integrity boundary: persisted WAL and SST bytes are verified before replay or read in `src/engine/wal.cpp` and `src/engine/sstable.cpp`

The current implementation also has important trust assumptions.

- The five logical Raft nodes are embedded inside one process through `kvstore::raft::TestCluster` and `TestTransport`, not through a separate networked peer transport, `src/service/kv_raft_service.cpp`, `src/raft/test_transport.cpp`
- Local process memory, the host filesystem, and the PEM files passed to `kvd` are trusted once the process starts
- This repository does not implement tenant isolation, user authentication, authorization, or automated secret handling

For v1, the main security goals are:

- protect client-facing gRPC traffic when `secure` mode is selected
- reject malformed or out-of-contract client requests before they enter the replicated command path
- detect persisted data corruption deterministically instead of silently accepting corrupted bytes

## Transport security profiles

The runtime listener supports exactly two profiles, `dev` and `secure`, in `src/bin/kvd.cpp`.

| Profile | Current behavior | Security meaning | Limits |
|---|---|---|---|
| `dev` | `kvd` uses `grpc::InsecureServerCredentials()` | Plaintext gRPC over TCP for local development and test flows | No transport encryption, no peer authentication beyond normal network reachability |
| `secure` | `kvd` uses `grpc::SslServerCredentials(...)` and requires `--tls_cert=PATH` plus `--tls_key=PATH` | Encrypts the client-facing gRPC listener with PEM certificate and private key material | No mTLS, no automated certificate rotation, no cluster-wide or inter-node TLS |

Task 7 runtime evidence confirms that both profiles start successfully and preserve the same `Put` and `Get` semantics through an external client smoke path, `tests/grpc/tls_profile_toggle_test.cpp` and `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`.

The secure-profile boundary is intentionally narrow.

- It covers the client-facing gRPC listener only
- It uses PEM cert and key files supplied at process start
- It does not imply full end-to-end cluster encryption
- It does not add mTLS, user authn, authz, or secret lifecycle automation

This narrow scope is consistent with the implemented topology in `docs/architecture.md`: peer replication is in-process in the current tree, so there is no separate inter-node TLS layer to enable today.

## Integrity and corruption detection

ADR 0003 establishes the v1 integrity contract: checksum verification is mandatory for WAL records and SST blocks on read and replay paths, `docs/adr/0003-integrity-checksum-strategy-v1.md`.

The implementation enforces that contract in two places.

### WAL replay

`src/engine/wal.cpp` verifies each replayed record before the mutation is accepted.
Replay rejects at least these cases:

- invalid WAL magic
- unsupported WAL version
- unknown operation tag
- record sizes that exceed the v1 contract limits
- truncated headers or payloads
- checksum mismatch

If checksum verification fails, replay returns an explicit integrity error instead of applying the record.

### SST open and read

`src/engine/sstable.cpp` verifies SST structure before data is trusted.
The reader checks:

- SST header magic and version
- footer magic and version
- footer checksum
- index-frame structure and checksum
- block-frame structure and checksum
- monotonic index ordering and payload bounds

If any of those checks fail, the read path surfaces an integrity error and does not return corrupted data as valid state.

### Evidence

Task 7 preserved corruption-injection evidence in `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`, produced by `scripts/integrity/run_corruption_suite.py`.
That artifact records `pass=true` and shows both of the repository's corruption gates failing closed:

- WAL replay returns `"integrity_code":"CHECKSUM_MISMATCH"`
- SST read returns `"integrity_code":"CHECKSUM_MISMATCH"`

This is the current proof that v1 detects on-disk corruption in the implemented durability path rather than silently accepting corrupted persisted bytes.

## Input validation and request-size limits

The v1 input contract is enforced in the service layer, not by protobuf alone, `proto/kvstore/v1/kv.proto`, `src/service/kv_raft_service.cpp`, and `docs/architecture/scope.md`.

Current limits and checks are:

- key size: `<= 1024` bytes for `Put`, `Get`, and `Delete`
- value size: `<= 1,048,576` bytes for `Put`
- `Put.request_id`: must be non-empty and `<= 4096` bytes
- conflicting reuse of a `Put.request_id` with different key or value bytes is rejected

When those checks fail, the gRPC adapter maps the service error to `INVALID_ARGUMENT`, `src/api/grpc_kv_service.cpp`.

Validation boundaries worth calling out explicitly:

- `Put` validates key, value, and `request_id` before proposal
- `Delete` validates key and requires a client-supplied `request_id` before proposal, matching the write-idempotency boundary in the service layer
- `Get` validates key size and rejects linearizable reads when the leader has no quorum contact, which prevents stale reads from being presented as current state
- WAL replay also rejects record sizes above the same v1 limits before reconstructing persisted commands, `src/engine/wal.cpp`

These checks create a clear boundary between client input and replicated state: malformed, oversized, or conflicting requests are rejected before they are treated as valid commands.

## Operational guidance for secure mode

For the current repository, secure operation means enabling the client-facing TLS listener and managing PEM files carefully by hand.

Use these rules:

1. Start `kvd` with `--tls_profile=secure --tls_cert=PATH --tls_key=PATH`, as required by `src/bin/kvd.cpp`.
2. Distribute the corresponding certificate material to clients that need to validate the server. The Task 7 smoke client does this through `--tls_cert=PATH` in `tests/grpc/tls_profile_toggle_test.cpp`.
3. Treat the certificate and private key files as operator-managed secrets. The repository does not rotate them, store them in a secret manager, or provision them automatically.
4. Use `secure` for any environment where plaintext client traffic is not acceptable. Keep `dev` for local development and controlled test scenarios only.
5. Do not describe `secure` mode as cluster encryption. It protects the client-facing listener only.

Operationally, secure mode improves confidentiality and integrity for client-facing RPC transport. It does not change the trust assumptions around embedded Raft peers, local disk contents, or process-local secret handling.

## Evidence and validation

This document is based on the following implemented sources.

### Code and ADR references

- `docs/adr/0003-integrity-checksum-strategy-v1.md`
- `src/bin/kvd.cpp`
- `src/service/kv_raft_service.cpp`
- `src/api/grpc_kv_service.cpp`
- `src/engine/wal.cpp`
- `src/engine/sstable.cpp`
- `proto/kvstore/v1/kv.proto`
- `docs/architecture.md`
- `docs/wire-protocol.md`
- `docs/testing.md`
- `docs/architecture/scope.md`

### Test and runtime evidence

- `tests/grpc/tls_profile_toggle_test.cpp`
- `scripts/integrity/run_corruption_suite.py`
- `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
- `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`

The key evidence points are:

- Task 7 runtime TLS smoke coverage recorded `pass=true` for both `dev` and `secure` listener profiles in `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
- `tests/grpc/tls_profile_toggle_test.cpp` verifies semantic parity across the two transport profiles and reports `"semantic_drift":false` in the in-process coverage path
- `scripts/integrity/run_corruption_suite.py` and `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json` show WAL and SST corruption surfacing as `CHECKSUM_MISMATCH`

## Explicit gaps / non-goals

The current repository does not implement the following security features, and this document does not imply otherwise.

- inter-node TLS or a separate TLS-protected Raft peer transport
- mutual TLS
- authentication or authorization
- multi-tenant isolation
- secret rotation
- key management automation
- encryption at rest
- production PKI lifecycle management

Secure mode in v1 should therefore be read precisely:

- yes, it encrypts the client-facing gRPC transport using PEM cert and key files
- yes, it has runtime evidence in Task 7
- no, it does not provide full end-to-end cluster encryption
- no, it does not provide identity, access-control, or tenancy isolation guarantees
- no, it does not replace future hardening work for deployment, secret storage, or certificate operations
