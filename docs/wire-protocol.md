# KVStore v1 Wire Protocol

## Purpose

This document describes the client-visible wire contract for the current `kvstore.v1` gRPC API. It records what the shipped v1 server accepts and returns today, based on `proto/kvstore/v1/kv.proto`, `src/api/grpc_kv_service.cpp`, `src/service/kv_raft_service.cpp`, and the current gRPC tests.

This is a unary-only contract. v1 exposes `Put`, `Get`, and `Delete` only. It does not expose `Txn`, `Watch`, `Lease`, `RangeScan`, or any streaming RPCs on the wire.

The repository now also contains an internal peer gRPC protocol in `proto/kvstore/v1/raft.proto`, but that is a node-to-node transport contract, not part of the public client API described in this document.

## Service and RPC inventory

Source of truth: `proto/kvstore/v1/kv.proto:8-39`.

| RPC | Request | Response | Current v1 behavior |
|---|---|---|---|
| `Put` | `PutRequest` | `PutResponse` | Writes one key/value pair through the leader and waits for commit before replying, `src/api/grpc_kv_service.cpp:50-62`, `src/service/kv_raft_service.cpp:369-460` |
| `Get` | `GetRequest` | `GetResponse` | Performs a linearizable read only when the serving node is leader and still has quorum contact, `src/api/grpc_kv_service.cpp:64-77`, `src/service/kv_raft_service.cpp:530-562` |
| `Delete` | `DeleteRequest` | `DeleteResponse` | Deletes one key through the leader and waits for commit before replying, `src/api/grpc_kv_service.cpp:79-94`, `src/service/kv_raft_service.cpp:462-528` |

No additional RPCs are present in the proto, and no bidirectional, client-streaming, or server-streaming methods exist in v1.

## Message schemas and limits

Proto field definitions come from `proto/kvstore/v1/kv.proto:14-39`. Byte and string limits are enforced by the service layer, not by protobuf itself, in `src/service/kv_raft_service.cpp:22-23` and `src/service/kv_raft_service.cpp:377-387`, `469-474`, `534-535`.

### `PutRequest`

```proto
message PutRequest {
  bytes key = 1;
  bytes value = 2;
  string request_id = 3;
}
```

- `key` is opaque bytes. Current v1 limit is 1024 bytes.
- `value` is opaque bytes. Current v1 limit is 1 MiB, 1,048,576 bytes.
- `request_id` is a client-supplied identity string for write idempotency. It must be non-empty and at most 4096 bytes.

### `PutResponse`

```proto
message PutResponse {
  bool overwritten = 1;
}
```

- `overwritten=false` means the key did not previously exist.
- `overwritten=true` means the key existed before this write.
- This behavior is exercised in `tests/grpc/grpc_integration_test.cpp:67-106` and remains stable across a duplicate idempotent retry in `tests/grpc/grpc_idempotency_test.cpp:51-93`.

### `GetRequest`

```proto
message GetRequest {
  bytes key = 1;
}
```

- `key` is opaque bytes.
- Current v1 limit is 1024 bytes.

### `GetResponse`

```proto
message GetResponse {
  bool found = 1;
  bytes value = 2;
}
```

- `found=true` means the key was present and `value` is populated.
- `found=false` means the key was absent. The current adapter only sets `value` when `found=true`, `src/api/grpc_kv_service.cpp:71-76`.
- Success and not-found behavior are exercised in `tests/grpc/grpc_integration_test.cpp:81-130`.

### `DeleteRequest`

```proto
message DeleteRequest {
  bytes key = 1;
  string request_id = 2;
}
```

- `key` is opaque bytes.
- Current v1 limit is 1024 bytes.
- `request_id` is a client-supplied identity string for delete idempotency. It must be non-empty and at most 4096 bytes because `Delete` reuses the same service-layer validation boundary as other write identities.

### `DeleteResponse`

```proto
message DeleteResponse {
  bool deleted = 1;
}
```

- `deleted=true` means the key existed before the delete was applied.
- `deleted=false` means the key was already absent when the delete was applied, as shown by the service path storing `DeleteResult{.deleted = existed}`, `src/service/kv_raft_service.cpp:324-330`.
- A successful delete of an existing key is exercised in `tests/grpc/grpc_integration_test.cpp:108-130`.

## Status/error mapping

The gRPC adapter translates service errors in `src/api/grpc_kv_service.cpp:9-26`. The concrete mappings asserted by `tests/grpc/api_status_test.cpp:19-63` are:

| Service error code | gRPC status | Current message behavior | Evidence |
|---|---|---|---|
| `kInvalidArgument` | `INVALID_ARGUMENT` | Passes through the service message | `src/api/grpc_kv_service.cpp:12-13`, `tests/grpc/api_status_test.cpp:23-30` |
| `kNotLeader` | `FAILED_PRECONDITION` | Returns `e.message + " (leader_hint=<n>)"` in the status message | `src/api/grpc_kv_service.cpp:14-18`, `tests/grpc/api_status_test.cpp:32-43` |
| `kUnavailable` | `UNAVAILABLE` | Passes through the service message | `src/api/grpc_kv_service.cpp:18-19`, `tests/grpc/api_status_test.cpp:45-52` |
| `kTimeout` | `DEADLINE_EXCEEDED` | Passes through the service message | `src/api/grpc_kv_service.cpp:20-21`, `tests/grpc/api_status_test.cpp:54-61` |
| `kInternal` and default | `INTERNAL` | Passes through the service message | `src/api/grpc_kv_service.cpp:22-24` |

Current v1 status behavior to call out explicitly:

- Validation failures surface as `INVALID_ARGUMENT`. Examples include oversized keys, oversized values, empty `request_id`, oversized `request_id`, and `Put` retries that reuse a `request_id` with different key or value bytes, `src/service/kv_raft_service.cpp:377-387`, `399-415`, `469-474`, `534-535`, `tests/grpc/grpc_idempotency_test.cpp:81-93`.
- Not-leader responses surface as `FAILED_PRECONDITION`, with the leader hint embedded in the status message string today. v1 does not expose structured leader-hint metadata in headers, trailers, or protobuf fields.
- Quorum loss and leader unavailability surface as `UNAVAILABLE`, including linearizable read rejection when the leader has no quorum contact, `src/service/kv_raft_service.cpp:430-448`, `497-516`, `540-548`, `tests/grpc/grpc_integration_test.cpp:136-199`.
- Commit wait timeout surfaces as `DEADLINE_EXCEEDED`, `src/service/kv_raft_service.cpp:453-458`, `521-526`, `tests/grpc/api_status_test.cpp:54-61`.

## Idempotency and request identity

### `Put`

`Put` is a write RPC with a client-supplied wire-level request identity. The server treats `request_id` as the idempotency key, `src/service/kv_raft_service.cpp:383-387`, `399-427`.

- A duplicate `Put` with the same `request_id`, key, and value returns the original logical result instead of applying a second write.
- A duplicate in flight shares the same completion future.
- Reusing a `request_id` with different key or value bytes is rejected as `INVALID_ARGUMENT`.

This client-visible behavior is exercised in `tests/grpc/grpc_idempotency_test.cpp:32-97`.

### `Delete`

`Delete` is client-idempotent on the wire in the current tree. `DeleteRequest` carries a client-supplied `request_id`, and `GrpcKvService::Delete` forwards that identity directly into `KvRaftService::Delete`.

That means a client retry after an uncertain network outcome can present the same delete identity back to the server. A duplicate `Delete` with the same `request_id` returns the original logical result instead of being treated as a distinct delete attempt at the service and WAL boundary.

### `Get`

`Get` is read-only and has no request identity field.

## Deadline and timeout behavior

Server-side deadline handling is implemented in `src/api/grpc_kv_service.cpp:30-46`.

- `Put` and `Delete` translate the incoming gRPC deadline into a `steady_clock` deadline and pass it to the service layer, `src/api/grpc_kv_service.cpp:53-55`, `82-87`.
- If the gRPC context has no deadline, the adapter currently uses a fallback of `now + 10s`, `src/api/grpc_kv_service.cpp:34-36`.
- If the incoming deadline is already expired, the adapter passes an immediate deadline, `src/api/grpc_kv_service.cpp:41-43`.
- The service then waits for commit with `wait_until(deadline)` and returns `kTimeout` if commit is not ready by that point, `src/service/kv_raft_service.cpp:453-458`, `521-526`.
- The adapter maps that timeout to `DEADLINE_EXCEEDED`, `src/api/grpc_kv_service.cpp:20-21`, `tests/grpc/api_status_test.cpp:54-61`.

`Get` does not currently consume the transport deadline in the gRPC adapter. `GrpcKvService::Get` ignores `ServerContext`, and the service API has no deadline parameter for reads, `src/api/grpc_kv_service.cpp:64-67`, `src/service/kv_raft_service.cpp:530-562`.

This section describes server-side behavior in the current tree. Client libraries may also enforce their own local gRPC deadlines or cancellations outside this adapter logic.

## Transport profiles, dev/plaintext and secure/TLS

Runtime profile handling is defined in `src/bin/kvd.cpp:15-145`.

### `dev`

- Default profile.
- Listener uses `grpc::InsecureServerCredentials()`, which is plaintext gRPC over TCP, `src/bin/kvd.cpp:95-98`.
- External clients use `grpc::InsecureChannelCredentials()` in the Task 7 profile toggle test, `tests/grpc/tls_profile_toggle_test.cpp:228-235`.

### `secure`

- Listener uses `grpc::SslServerCredentials(...)`, `src/bin/kvd.cpp:98-121`.
- Server startup requires both `--tls_cert=PATH` and `--tls_key=PATH`, `src/bin/kvd.cpp:99-116`.
- External clients use `grpc::SslCredentials(...)` with the provided certificate material, `tests/grpc/tls_profile_toggle_test.cpp:213-225`, `228-235`.

### Runtime evidence from Task 7

Task 7 recorded successful external smoke coverage for both profiles in `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`:

- `dev` profile passed against `127.0.0.1:44657` with `tls_profile=dev`, lines 11-40.
- `secure` profile passed against `127.0.0.1:41985` with `tls_profile=secure`, lines 42-73.

The same run also records `pass=true` for the overall TLS profile toggle payload. `tests/grpc/tls_profile_toggle_test.cpp:246-299` verifies that both profiles preserve the same `Put` and `Get` semantics, and prints `"semantic_drift":false` for the in-process profile coverage.

## Compatibility / non-goals

- Package and service namespace are fixed as `kvstore.v1.KV` in the current proto, `proto/kvstore/v1/kv.proto:3-12`.
- v1 is intentionally minimal. It does not expose `Txn`, `Watch`, `Lease`, `RangeScan`, membership change APIs, or any streaming RPCs on the wire.
- The server may include `leader_hint=<n>` inside a `FAILED_PRECONDITION` message string for not-leader errors, but v1 does not define structured leader-hint metadata.
- Protobuf limits are not self-describing in the schema. Clients must follow the current server-enforced limits documented here.
- `Delete` request identity is part of the wire contract in the current tree through `DeleteRequest.request_id`.

## Validation and evidence

### Code references used for this document

- Proto contract: `proto/kvstore/v1/kv.proto`
- gRPC adapter and status translation: `src/api/grpc_kv_service.cpp`
- Service validation, leader gating, idempotency, and commit waiting: `src/service/kv_raft_service.cpp`
- Runtime listener profile handling: `src/bin/kvd.cpp`

### Test and runtime evidence cited

- Status mapping: `tests/grpc/api_status_test.cpp`
- End-to-end RPC semantics and quorum rejection: `tests/grpc/grpc_integration_test.cpp`
- `Put` idempotency behavior: `tests/grpc/grpc_idempotency_test.cpp`
- Transport profile semantic parity: `tests/grpc/tls_profile_toggle_test.cpp`
- Task 7 TLS and partition-heal runtime evidence: `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
- Task 7 post-fix runtime stability evidence: `.sisyphus/evidence/task7-post-fix/task-7-bench.json`, which records `pass=true`, `samples=300`, and `no_acknowledged_write_loss=true`

The document should be updated when the proto changes, when a new client-visible status mapping is added, or when v1 grows new wire-visible request identity or transport behavior.
