# KVStore v1 Operations

## Purpose

This document explains how to build, start, operate, and validate the current KVStore v1 runtime that exists in this repository today.
It is limited to the implemented `kvd` binary, the current client-facing gRPC listener, the unary `Put`, `Get`, and `Delete` API surface documented in `docs/wire-protocol.md`, the embedded in-process Raft topology, and the Task 7 runtime evidence.
It does not claim deployment assets or runtime capabilities that are not present in the tree.

## Runtime model in v1

`kvd` is the only server entry point in the current repository, `src/bin/kvd.cpp`.
One `kvd` process exposes one client-facing gRPC endpoint and constructs `kvstore::service::KvRaftService` over a static in-process five-node Raft cluster.
Those five logical nodes are embedded through `TestCluster` and `TestTransport`, not as five separate OS processes and not as a separate peer network, `docs/architecture.md`, `src/service/kv_raft_service.cpp`.

Current operator-facing flags are exactly:

- `--listen_addr=`
- `--data_dir=`
- `--tls_profile=dev|secure`
- `--tls_cert=`
- `--tls_key=`

Current listener behavior from `src/bin/kvd.cpp`:

- `--listen_addr=` defaults to `127.0.0.1:50051`
- `--data_dir=` defaults to `./data`
- `--tls_profile=dev|secure` defaults to `dev`
- `--tls_profile=dev` uses plaintext gRPC over TCP
- `--tls_profile=secure` requires both `--tls_cert=` and `--tls_key=` or startup fails with exit code `1`

This is the v1 runtime topology to keep in mind during operations:

- one process
- one external gRPC listener
- five static voting Raft nodes embedded in that process
- one local storage subtree per logical node under `data_dir/node*`

## Build and start prerequisites

The current tree builds with CMake at the repository root, `CMakeLists.txt`, and requires a C++20-capable toolchain.
gRPC service support is enabled by default through `KVSTORE_ENABLE_GRPC=ON`, and `src/CMakeLists.txt` builds `kvd` plus the gRPC and integration test binaries.

Practical prerequisites for the current repo state:

- CMake 3.16 or newer, `CMakeLists.txt:1`
- a C++20 compiler, `CMakeLists.txt:5-7`
- a build environment that can satisfy the repository's gRPC and protobuf runtime requirements, `src/CMakeLists.txt:23-108`
- `python3` for the validation wrappers under `scripts/chaos/` and `scripts/bench/`
- a shell environment that can run the repository scripts. Task 7 runtime validation was executed through WSL-style paths, which should be treated as the validation environment used for evidence capture, not as a product requirement, `docs/testing.md`

The repository's local build baseline is:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
(cd build && ctest --output-on-failure)
```

That baseline comes directly from `scripts/ci/local_check.sh`.

## Startup commands, dev/plaintext and secure/TLS

### Development or plaintext listener

```bash
./build/src/kvd \
  --listen_addr=127.0.0.1:50051 \
  --data_dir=./data \
  --tls_profile=dev
```

Use this mode when you want the default local listener profile and do not need TLS on the client-facing endpoint.
This is the default profile in `src/bin/kvd.cpp` and the form shown in `deploy/kvd_single_process.conf`.

### Secure TLS listener

```bash
./build/src/kvd \
  --listen_addr=127.0.0.1:50051 \
  --data_dir=./data \
  --tls_profile=secure \
  --tls_cert=./tests/assets/tls/server.crt \
  --tls_key=./tests/assets/tls/server.key
```

Use this mode when you want TLS on the client-facing gRPC endpoint.
`src/bin/kvd.cpp` reads both PEM files at startup. If either file is missing or unreadable, startup fails before the server begins listening.

Task 7 external smoke evidence used the same secure profile shape with repository test certificates, `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`.

## Data directory layout and persistence assumptions

`kvd` creates the requested `data_dir` and then creates one storage directory per logical Raft node:

- `data_dir/node1/`
- `data_dir/node2/`
- `data_dir/node3/`
- `data_dir/node4/`
- `data_dir/node5/`

This layout is created in `src/service/kv_raft_service.cpp:199-223`.
Each logical node opens its own local engine with WAL-backed state rooted in its node directory. The initial WAL path is `nodeN/000001.wal`, and SST files may appear later as the engine flushes or compacts state.

Operational assumptions for persistence in the current v1 tree:

- storage is local to the host running `kvd`
- persistence is per logical node, even though all five nodes live inside one process
- restarting `kvd` against the same `--data_dir=` reuses the existing local state
- deleting or replacing `--data_dir=` resets the embedded cluster's persisted state

There is no runtime support for remote shared storage, dynamic membership rebalancing, or multi-process cluster orchestration in the current repository.

## Operational checks and expected startup output

Successful startup prints one line to stdout from `src/bin/kvd.cpp`:

```text
kvd listening on <effective_listen_addr> (requested=<listen_addr>, tls_profile=<profile>, data_dir=<path>)
```

Example operational checks after launch:

1. Confirm the process stays alive.
2. Confirm the startup line includes the expected `requested=`, `tls_profile=`, and `data_dir=` values.
3. Confirm the listener accepts TCP connections on `--listen_addr=`.
4. Confirm `data_dir/node1` through `data_dir/node5` were created.
5. Run the external TLS/profile smoke client when you need proof that the live listener can serve `Put` and `Get`.

Expected startup failures and messages from `src/bin/kvd.cpp`:

- unsupported TLS profile, `unsupported --tls_profile=<value>; expected dev or secure`
- missing secure credentials, `secure TLS profile requires both --tls_cert=PATH and --tls_key=PATH`
- unreadable certificate, `failed to read TLS certificate PEM from ...`
- unreadable key, `failed to read TLS private key PEM from ...`
- listener bind failure, `failed to start gRPC server on ...`

## Failure handling and recovery expectations

The current v1 runtime is a single process with an embedded static five-node cluster.
That means failure handling is mostly about leader replacement, quorum loss behavior, and restart recovery inside that single process model.

Evidence-backed expectations from Task 7:

- leader failover is expected to preserve service continuity when a leader is removed but quorum remains available. Observed result: `pass=true`, `leader_before=4`, `leader_after=3`, `failover_ms=10`, threshold `max_failover_ms=5000`, `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`
- restart recovery is expected to reopen persisted state from the same `--data_dir=` and recover within the repository acceptance window. Observed result: `pass=true`, `restart_rto_ms=1`, threshold `max_rto_ms=60000`, `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`
- partition-heal behavior is expected to reject writes when quorum contact is lost, then recover after healing. Observed result: `partition_heal.pass=true`, `partition_write_rejected=true`, `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`
- client-facing TLS profile switching is expected to preserve external `Put` and `Get` semantics in both `dev` and `secure` modes. Observed result: `tls_profile_toggle.pass=true`, with successful external smoke targets for both profiles in `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`

Operationally, if the process exits or becomes unhealthy, the supported recovery action in v1 is to restart the single `kvd` process against the same `--data_dir=`.
There is no documented or implemented v1 workflow for replacing individual embedded peers as separate services because the peers are not separate services.

## Evidence-backed validation commands

Use the commands below when you want to reproduce the current repository validation story with the same entry points that produced the saved Task 7 evidence.

### Build baseline

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
(cd build && ctest --output-on-failure)
```

### Failover evidence

```bash
python3 scripts/chaos/kill_leader_and_assert.py \
  --build-dir build \
  --evidence-dir .sisyphus/evidence/task7-checks/failover
```

Expected artifact: `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json`

### Restart recovery evidence

```bash
python3 scripts/chaos/assert_restart_rto.py \
  --build-dir build \
  --evidence-dir .sisyphus/evidence/task7-checks/restart
```

Expected artifact: `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`

### Partition-heal plus runtime TLS/profile evidence

```bash
python3 scripts/chaos/partition_heal_check.py \
  --build-dir build \
  --repo-root . \
  --evidence-dir .sisyphus/evidence/task7-final
```

Expected artifact: `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`

This wrapper launches `build/src/kvd` in both `--tls_profile=dev` and `--tls_profile=secure` modes, then runs `build/tests/grpc/kvstore_tls_profile_toggle_test` in external mode against the live listener.

### Benchmark and durability gate

```bash
scripts/bench/run_baseline.sh build --out .sisyphus/evidence/task7-post-fix/task-7-bench.json
python3 scripts/bench/assert_slo.py \
  --input .sisyphus/evidence/task7-post-fix/task-7-bench.json
```

The authoritative post-fix benchmark artifact is `.sisyphus/evidence/task7-post-fix/task-7-bench.json`, which records:

- `pass=true`
- `samples=300`
- `p99_durable_write_ms=1.416`
- `p99_read_ms=0.002`
- `no_acknowledged_write_loss=true`

Use that post-fix artifact as the current baseline, not the earlier failed benchmark artifact under `.sisyphus/evidence/task7-checks/bench/`.

## Deploy baseline

`deploy/kvd_single_process.conf` is explicit that v1 is a single-process test configuration with an in-process deterministic five-node Raft cluster.
`deploy/README.md` is also explicit that cluster and local deployment manifests will be added in later tasks.

For current operations, that means:

- this repo documents how to run `kvd` directly
- this repo does not yet ship full deployment manifests for cluster or local orchestration
- operators should treat deployment automation as not yet present in the repository baseline

## Known limitations / non-goals

The following are not part of the current v1 operations story and should not be inferred from the current tree:

- no multi-process Raft cluster orchestration
- no Kubernetes, systemd, or docker-compose assets in this repository baseline
- no separate inter-node TLS layer, because Raft peer messaging is in-process in v1
- no dynamic membership add, remove, or reconfigure flows
- no transaction, watch, lease, range scan, or streaming RPC operations
- no claim that the Task 7 WSL-backed validation path is a product runtime requirement

If the runtime model changes in a later task, this document should be updated alongside `src/bin/kvd.cpp`, deploy artifacts, and the saved evidence set.
