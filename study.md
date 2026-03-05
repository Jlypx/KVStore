# 学习笔记（Tasks 1-3）

> 目标：补齐“已实现但未提交”的版本管理缺口，把 Wave 1 的三步工作以 **3 个原子提交**落盘，便于后续 Task 4+ 持续演进。

## Task 1 - 初始化仓库工程基线（C++20 + CMake + CI）

### 我做了什么
- 建立 C++20 工程的基础构建/测试骨架（CMake + CTest），并提供本地 CI 脚本与 GitHub Actions 基线。
- 引入静态检查与格式化基线（clang-tidy / clang-format 配置）。

### 常用命令（验收/复现）
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

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
ctest --test-dir build --output-on-failure

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
