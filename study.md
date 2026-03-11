# 学习笔记（Tasks 1-9）

> 目标：补齐“已实现但未提交”的版本管理缺口，把 Wave 1 的三步工作以 **3 个原子提交**落盘，便于后续 Task 4+ 持续演进。

## Task 1 - 初始化仓库工程基线（C++20 + CMake + CI）

### 我做了什么
- 建立 C++20 工程的基础构建/测试骨架（CMake + CTest），并提供本地 CI 脚本与 GitHub Actions 基线。
- 引入静态检查与格式化基线（clang-tidy / clang-format 配置）。

### 常用命令（验收/复现）
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
(cd build && ctest --output-on-failure)

# 一键本地检查（如果仓库提供）
scripts/ci/local_check.sh
```

### 学到的点
- **Out-of-source build** 是团队协作的默认约定：源目录保持干净，所有产物进 `build/`。
- CTest 在较老版本（如 3.16）上存在 CLI 兼容性差异，需要用仓库内的脚本/兼容 shim 兜底，确保 CI 与本地一致。

## Task 2 - 冻结 v1 合约与 ADR（Architecture Contracts）

### 我做了什么
- 冻结 v1 的接口/边界合约（proto + scope 文档）。
- 冻结关键决策（ADR 集合），把“做什么 / 不做什么 / 为什么”写成可审计的架构契约。
- 提供 scope/proto 的确定性检查脚本，避免后续开发越界。

### 常用命令（验收/复现）
```bash
# Scope 合约检查
scripts/ci/check_scope_contract.sh

# proto 合约编译检查（优先使用仓库内自举的工具）
scripts/ci/bootstrap_proto_tools.sh
scripts/ci/check_proto_contract_compile.sh
```

### 学到的点
- “冻结合约”不是写一份文档就结束：需要 **可执行的检查脚本**，将契约变成可持续的 CI gate。
- proto 工具链在不同环境差异大，仓库内自举（repo-local sysroot）能减少对全局环境的依赖，但要配合 `.gitignore` 防止把下载产物提交进版本库。

## Task 3 - WAL + MemTable + recovery + checksum（Engine v1 durability）

### 我做了什么
- 实现 WAL 记录格式（header + payload + checksum）与启动时 replay/recovery。
- 引入完整性模块（CRC32C/错误分类），并提供 WAL 损坏注入脚本用于验证 checksum 检测。
- 增加与 Task 3 对应的测试用例，覆盖：WAL round-trip、校验失败、崩溃恢复重放、请求幂等。

### 常用命令（验收/复现）
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
(cd build && ctest --output-on-failure)

# WAL 损坏注入（如果仓库提供）
python3 scripts/integrity/corrupt_wal_byte.py --help
```

### 学到的点
- 数据完整性要“默认开启”：checksum 验证应在 replay/读路径第一时间 fail-fast。
- recovery 的验收要靠测试证明（重启后的状态一致性），不能只靠人工推理。

## 版本管理（本次补齐的提交策略）

- 分支约定：默认分支对齐为 `main`（unborn 分支用 `git symbolic-ref` 切换）。
- 忽略策略：通过 `.gitignore` 忽略 `.sisyphus/`（证据/笔记）与 `.tools/`（工具链下载/sysroot），以及 `build/`（构建产物）。
- 提交拆分：
  1) `chore(repo): bootstrap c++20 toolchain and ci baseline`
  2) `docs(architecture): freeze v1 contracts and adrs`
  3) `feat(engine): add wal replay and checksum validation`

## Task 4 - SSTable/BlockCache/Compaction（Engine 读放大治理）

### 分步记录（补记）
1. 在 `engine/sstable.*` 落地 v1 SSTable 读写路径，保证文件级校验与错误透传。
2. 在 `cache/block_cache.*` 引入块缓存，优先命中热点块，减少重复磁盘读取。
3. 在 `engine/compaction.*` 实现基础 compaction 流程（多 SST 合并到单 SST）。
4. 在 `KvEngine` 中接线 `Flush()` / `Compact()`，把内存态与文件态串起来。
5. 补齐 `sstable_test` / `cache_test` / `compaction_test` 回归，验证读写正确性与压缩后可读性。

### 关键收获
- v1 阶段先做“可验证的最小 compaction”，比一开始追求复杂策略更稳。
- 块缓存命中与完整性校验要同时成立：不能为了性能绕过校验链路。

## Task 5 - Raft Core（静态 5 节点共识）

### 分步记录（补记）
1. 在 `raft/raft_node.*` 完成 Follower/Candidate/Leader 状态机与 Tick 驱动超时机制。
2. 在 `raft/test_transport.*` 实现内存消息总线与 `TestCluster`，支持可控投递/故障注入。
3. 实现 `Propose`、日志复制、提交推进与 `on_committed` 回调。
4. 增加 quorum-contact gate：无多数派最近心跳时，拒绝写入提案。
5. 通过 `raft_election/replication/quorum/failover` 测试验证安全性与故障切换行为。

### 关键收获
- 纯 Tick 驱动 + 内存传输层可稳定复现实验场景，适合 v1 迭代速度。
- “可提交”与“可响应客户端”要分开考虑，quorum-contact gate 是关键防线。

## Task 6 - gRPC KV Service × Raft 状态机集成（当前）

### 分步记录
1. 保持 `proto/kvstore/v1/kv.proto` 不扩展，只接 `Put/Get/Delete` 三个 unary RPC。
2. 在 `service/kv_raft_service.cpp` 完成传输命令编码/解码、Raft propose、commit apply 到 `KvEngine` 的闭环。
3. 修复命令解码顺序缺陷（长度字段与 payload 读取顺序），消除写入超时问题。
4. 强化启动准备：服务初始化阶段等待 leader 选举与 quorum-contact 预热，避免首个写请求误失败。
5. 在 `api/grpc_kv_service.*` 建立错误映射：
   - `kNotLeader -> FAILED_PRECONDITION`
   - `kUnavailable -> UNAVAILABLE`
   - `kTimeout -> DEADLINE_EXCEEDED`
6. 新增 `tests/grpc/api_status_test.cpp`，验证 API 层状态码映射；扩展 `grpc_integration_test` 覆盖 quorum 丢失下 Put/Get 行为。
7. 补齐 CTest 兼容 shim 与 gRPC 运行时加载策略（RPATH + test ENV），确保老版本 CTest 场景也能执行 gRPC/integration 用例。
8. 增强 `scripts/ci/bootstrap_grpc_runtime.sh` 的 SSL 运行时包选择逻辑（`libssl3`/`libssl1.1` 兼容）。

### 关键收获
- 服务层是“传输协议”和“领域语义”的缓冲带：Raft/Engine 不应依赖 gRPC 头文件。
- 线性一致读在工程上要落到可执行条件（leader + quorum-contact），不能只写在 ADR。

## Task 7 - chaos / integrity / performance 验收闭环 + 运行时 TLS/profile smoke

### 目标
- 把 Task 5/6 已有的 Raft、存储、gRPC 能力收成一套可复现、可落盘的验收闭环，不只看测试绿，还要留下 `.sisyphus/evidence/` 里的 JSON / log 证据。
- 覆盖 failover、restart、integrity、partition-heal、runtime TLS/profile smoke、benchmark 六类结果，并修掉 benchmark 暴露出来的 acknowledged-write-loss 问题。

### 分步记录 / 我做了什么
1. 用 `tests/integration/chaos_gate_test.cpp` 配合 `scripts/chaos/kill_leader_and_assert.py`、`scripts/chaos/assert_restart_rto.py`，把 leader failover 和 restart RTO 变成可落盘的检查。
2. 落盘的结果分别是 `.sisyphus/evidence/task7-checks/failover/chaos_kill_leader_and_assert.json` 和 `.sisyphus/evidence/task7-checks/restart/chaos_assert_restart_rto.json`，当前记录为 `failover_ms=10`、`restart_rto_ms=1`，都在阈值内。
3. 用 `scripts/integrity/run_corruption_suite.py` 驱动 `kvstore_integrity_gate_test`、`scripts/integrity/corrupt_wal_byte.py`、`scripts/integrity/corrupt_sst_block.py`，把 WAL / SST 损坏检测固定成 `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json`。
4. 这份完整性证据里明确写出了两条 fail-closed 路径，`wal_corruption.replay_payload.integrity_code="CHECKSUM_MISMATCH"`，`sst_corruption.read_payload.integrity_code="CHECKSUM_MISMATCH"`，说明坏数据没有被静默吞掉。
5. 用 `scripts/chaos/partition_heal_check.py` 先跑 `kvstore_chaos_gate_test partition_heal`，再拉起真实 `build/src/kvd` 做外部 smoke，把 partition-heal 和 runtime TLS/profile 覆盖合并到 `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`。
6. 这份最终证据里，`partition_heal.pass=true`、`partition_write_rejected=true`，同时 `dev target=127.0.0.1:44657` 和 `secure target=127.0.0.1:41985` 都是 `smoke.pass=true`。
7. 用 `scripts/bench/run_baseline.sh` 和 `python3 scripts/bench/assert_slo.py` 固化性能与 durability gate，保留早期失败产物 `.sisyphus/evidence/task7-checks/bench/task-7-bench.json`，再以 `.sisyphus/evidence/task7-post-fix/task-7-bench.json` 作为修复后的权威基线。
8. post-fix bench 当前值是 `pass=true`、`samples=300`、`p99_durable_write_ms=1.416`、`p99_read_ms=0.002`、`no_acknowledged_write_loss=true`，说明 acknowledged-write-loss 修复已经落地。

### 常用命令（验收/复现）
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

python3 scripts/chaos/kill_leader_and_assert.py \
  --build-dir build \
  --evidence-dir .sisyphus/evidence/task7-checks/failover

python3 scripts/chaos/assert_restart_rto.py \
  --build-dir build \
  --evidence-dir .sisyphus/evidence/task7-checks/restart

python3 scripts/integrity/run_corruption_suite.py \
  --repo-root . \
  --build-dir build \
  --evidence-dir .sisyphus/evidence/task7-checks/integrity

python3 scripts/chaos/partition_heal_check.py \
  --build-dir build \
  --repo-root . \
  --evidence-dir .sisyphus/evidence/task7-final

scripts/bench/run_baseline.sh build --out .sisyphus/evidence/task7-post-fix/task-7-bench.json
python3 scripts/bench/assert_slo.py \
  --input .sisyphus/evidence/task7-post-fix/task-7-bench.json
```

### 遇到的问题
- 源码根目录的 CTest compatibility shim 一开始没有把 gRPC / integration 二进制都接进来，导致从根目录看起来“测试能跑”，但实际 coverage 有缺口，这个坑是在收 Task 7 验收面时暴露得更明显。
- 当前环境对 `libgrpc.so.6` 这类运行时库的解析不够稳定，真实拉起 `kvd` 和 gRPC smoke 时会踩 loader 问题，所以后来必须把 RPATH / test ENV 处理得更确定。
- `partition_heal` 不能只测“恢复后能写”，还得先证明分区期间写入被拒绝，否则用例很容易误把错误行为当成恢复成功。
- benchmark 的第一版产物 `.sisyphus/evidence/task7-checks/bench/task-7-bench.json` 直接报了 `{"pass":false,"reason":"acknowledged_write_lost"}`，这说明“已经回给客户端成功”的写入仍有丢失窗口，必须修。

### 学到的点 / 关键收获
- 验收闭环的关键不是多写几个测试，而是把结果变成机器可读的证据文件，这样后面写 release / docs gate 时才有可靠输入。
- runtime TLS/profile smoke 和 in-process 测试要分开理解，前者证明 `build/src/kvd` 真能以 `dev` / `secure` 起来并被外部 client 打通，不等于已经有独立 peer transport TLS。
- 失败产物不能随手删，早期 failed bench 正是定位 acknowledged-write-loss 的直接证据，保留下来更利于追问题。

### 下一步
- 继续把 Task 7 的证据路径接进后续 release / docs gate，避免后面只剩“文档说通过”，没有真实 artifact。
- 如果后面把嵌入式 `TestCluster` 换成多进程 peer transport，需要重写 partition / TLS 验证边界，不能直接复用当前结论。

## Task 8 - 文档补全与 docs gate 脚本

### 目标
- 把 Task 1-7 已经实现的架构、协议、测试、运维、安全、发布信息整理成仓库内可交付文档，不再只散落在源码、测试和脚本里。
- 给文档补最小门禁，确保关键文档、核心标题、命令块和 `.sisyphus/evidence/` 引用不会在后续提交里悄悄退化。

### 分步记录 / 我做了什么
1. 新增 `docs/architecture.md`，把代码边界、请求链路、模块边界、Task 7 运行证据统一到一份架构文档里。
2. 新增 `docs/wire-protocol.md`，把 `proto/kvstore/v1/kv.proto`、RPC 语义、状态码和 listener profile 说明写清楚。
3. 新增 `docs/testing.md`、`docs/operations.md`、`docs/security.md`、`docs/release.md`，分别沉淀测试分层、运行方式、安全边界、发布基线。
4. 更新 `docs/storage-format.md`，把 Task 7 的完整性验证接进存储格式说明，特别是 `.sisyphus/evidence/task7-checks/integrity/integrity_corruption_suite.json` 里的 `CHECKSUM_MISMATCH` 证据。
5. 新增 `scripts/docs/check_required_docs.sh`，检查 `docs/architecture.md`、`docs/wire-protocol.md`、`docs/testing.md`、`docs/operations.md`、`docs/security.md`、`docs/release.md`、`docs/storage-format.md` 是否存在，并检查核心标题是否在位。
6. 新增 `scripts/docs/check_command_blocks.sh`，专门检查 `docs/operations.md`、`docs/testing.md`、`docs/release.md` 里是否真的出现 build / test 命令、Task 7 验收命令，以及 `.sisyphus/evidence/task7-*` 路径。
7. 这一步不只是“把文档写多一点”，而是把 Task 7 的真实证据链写进文档，再用脚本把这些引用固定下来，避免后续文档变成空壳。

### 常用命令（验收/复现）
```bash
bash scripts/docs/check_required_docs.sh
bash scripts/docs/check_command_blocks.sh
```

- 这两条 gate 直接对应 Task 8 新增脚本：`scripts/docs/check_required_docs.sh`、`scripts/docs/check_command_blocks.sh`。
- 当前文档面覆盖的核心文件是 `docs/architecture.md`、`docs/wire-protocol.md`、`docs/testing.md`、`docs/operations.md`、`docs/security.md`、`docs/release.md`，以及更新后的 `docs/storage-format.md`。

### 遇到的问题
- 只补正文不够，因为 Task 8 不是单纯写文档，还要保证文档里真的有可执行命令和真实证据路径，不然 review 时很难判断哪些段落只是描述，哪些结论有落地依据。
- `docs/storage-format.md` 是已有文件，不是新建文件，补写时必须保留原有格式定义，只把 Purpose / Validation and evidence / Task 7 integrity evidence 接进去，不能把底层格式说明写乱。
- docs gate 不能只看“文件存在”，所以才会拆成一个检查必需文件与标题，一个检查命令块和 `.sisyphus/evidence` 引用，否则很容易出现空文档也过关。

### 学到的点 / 关键收获
- 文档也要有最小可执行验收线，Task 8 这两个脚本本质上就是给文档加了一个轻量 gate。
- 最有用的文档不是概念介绍，而是能把人从命令直接带到证据文件，比如从 `scripts/chaos/partition_heal_check.py` 走到 `.sisyphus/evidence/task7-final/chaos_partition_heal_and_tls.json`。
- `docs/testing.md`、`docs/operations.md`、`docs/release.md` 必须互相引用同一批命令和证据，不然一边写“怎么跑”，另一边写“凭什么算通过”，最后很容易对不上。

### 下一步
- 后续如果再加新的 gate、证据路径或运行方式，要同步更新这两份 docs gate 脚本和对应文档，不然很快会出现“代码变了，文档没跟上”。
- 等后续发布治理真正落地，再补 versioning、artifact policy、rollback 这类当前仍明确 deferred 的内容。

## Task 9 - release/governance 基线与仓库内门禁脚本

### 目标
- 把 Task 8 已经补齐的文档面收成“当前仓库到底能真实声明什么 release governance”的基线，明确哪些 policy 已经写实，哪些能力仍然只是 deferred，不能夸大。
- 增加仓库内可执行的提交信息门禁和 release readiness 聚合门禁，让 release claim 不再只靠人工 review 和记忆。

### 分步记录 / 我做了什么
1. 回头把 `docs/release.md` 定稿，补齐 `Release checklist and readiness gate`、`Versioning policy for the v1 line`、`Changelog policy and template guidance`、`Git governance, authorship, and branch strategy`、`Rollback and release notes expectations`、`Known release risks and deferred items` 这些最终治理内容。
2. 新增 `scripts/git/check_commit_messages.sh`，用正则把提交标题限制在 Conventional Commits 兼容格式，要求是小写 type、可选 scope、冒号后必须有正文。
3. 这个提交门禁脚本除了检查 subject，还专门处理了 `--range` 的解析问题，像 `HEAD~20..HEAD` 这种范围在短历史或当前分支场景里不一定总能直接解析，所以脚本会在左边界不可用时尽量回退到右侧可解析提交，减少因为历史长度差异导致的误报。
4. 新增 `scripts/release/check_release_readiness.sh`，把 `docs` gate、`study.md` timeline gate、recent commit subject gate、Task 7/8 证据检查串成一个仓库内可直接运行的 readiness 入口。
5. 这个 readiness 脚本不只检查文件存在，还会验证 `docs/release.md` 里真的出现治理标题、`jlypx`、`Co-authored-by:` 等 policy marker，并检查 Task 7 JSON 与 Task 8 log 中的关键 `pass` / `OK` 标记。
6. 真正做下来后，发现 `docs/release.md` 不是先写完就结束了。等 enforcement 脚本加进仓库以后，文档还得再更新一轮，把“仓库里现在有 repo-local helper，但这不等于 hosted branch protection 或 server-side governance 已经启用”这层边界补清楚，不然 release 文案会说过头。
7. Task 9 的两条验收输出路径也一起固定下来，分别是 `.sisyphus/evidence/task-9-commit-style.log` 和 `.sisyphus/evidence/task-9-release-gate.log`，后续复跑 gate 时可以直接把命令输出落到这两个证据日志里。

### 常用命令（验收/复现）
```bash
bash scripts/git/check_commit_messages.sh --range HEAD~20..HEAD
bash scripts/release/check_release_readiness.sh
```

- 提交信息门禁的输出证据路径：`.sisyphus/evidence/task-9-commit-style.log`
- release readiness 聚合门禁的输出证据路径：`.sisyphus/evidence/task-9-release-gate.log`
- 第二条命令会进一步串起 `scripts/docs/check_required_docs.sh`、`scripts/docs/check_command_blocks.sh`、`scripts/docs/check_study_timeline.py --expected-tasks 9`，以及 Task 7/8 已落盘证据的存在性与关键标记检查。

### 遇到的问题
- `docs/release.md` 的治理内容和 enforcement helper 不是两条平行线，脚本加进来以后必须回头再修文档一次，不然文档里的“当前已实现治理能力”会和仓库实际状态脱节。
- `scripts/release/check_release_readiness.sh` 明显受解释器环境影响，所以脚本里专门按 `python3 -> python -> py -3` 的顺序选 Python 3。这个兼容分支很实际，因为当前环境并不能假设每次都有同一个解释器名字可用。
- release readiness 检查的是“同一棵树上的文档、提交风格、study 时间线、Task 7/8 证据是否同时成立”，不是只看旧证据文件还在不在，所以只要文档或 gate 规则改了，就得重新对齐验证。

### 学到的点 / 关键收获
- 真正有用的 release 文档，不是泛泛介绍流程，而是把 SemVer、changelog、git authorship、branch strategy、rollback 边界、deferred 项写成脚本可引用的明确标记。
- `scripts/release/check_release_readiness.sh` 这类聚合 gate 的价值很高，因为它把 docs、study timeline、commit history、evidence artifact 串成一次检查，能减少“单项都看过了，但整体 release claim 仍然不真实”的情况。
- 治理脚本必须诚实反映仓库现状，可以声明 repo-local checks 已经存在，但不能把它包装成已经有 branch protection、server-side enforcement、artifact publication 或 signing/provenance 流程。

### 下一步
- 等 Task 9 的实际验收输出落盘后，把 `.sisyphus/evidence/task-9-commit-style.log` 和 `.sisyphus/evidence/task-9-release-gate.log` 接进后续 release note / review 证据链，避免又回到“文档说通过，但没有直接日志”的状态。
- 如果后面再补 branch protection、artifact publication、signing/provenance、upgrade/support-window 之类真正落地的治理能力，要先更新 `docs/release.md`，再同步扩展 `scripts/release/check_release_readiness.sh`，保持文档和门禁一起演进。
