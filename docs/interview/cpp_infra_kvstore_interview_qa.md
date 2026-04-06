# KVStore 项目面试题库（C++ / 基础架构向，当前版）

> 这份题库基于 `kvstore-hardening` 分支的**当前实现**整理，不再以早期“单进程 embedded 原型”作为主叙事。
> 现在这个项目已经具备：
> `WAL + MemTable + SSTable + Compaction + Block Cache + Tombstone + WAL Rotation + Stronger Sync + Durable Raft Metadata/Log + Snapshot + InstallSnapshot + embedded/cluster-node 双模式 + 同机多进程真 5 节点集群 + 本地 launcher + 多进程 failover/snapshot catch-up 测试`

## 一、项目当前定位

### 1. 现在你会怎么用 1 分钟介绍这个项目？
**参考答案：**  
这是一个我从零实现并持续演进的 C++ KVStore 原型，重点不是“做个 KV 容器”，而是把存储引擎、一致性、RPC 服务和验证闭环做完整。底层我实现了 WAL、MemTable、SSTable、Compaction、Block Cache、tombstone 和 checksum 校验；一致性层实现了 Raft 的选主、日志复制、提交推进、quorum gate、持久化元数据和日志；在此基础上又把运行时从最初的单进程 embedded 5 节点，演进到了 `cluster-node` 多进程真 5 节点模式，增加了 peer gRPC、NetworkTransport、本地 launcher，以及 snapshot 和 InstallSnapshot catch-up。这个项目比较能体现我做 C++ 基础架构、存储系统和分布式系统原型的能力。

### 2. 这个项目现在的最准确定义是什么？
**参考答案：**  
我会把它定义成“支持 embedded 模式和同机多进程 cluster-node 模式的 KVStore 原型”，而不是生产级数据库。它已经具备比较完整的语义链路和验证体系，但依然是一个工程原型，不是完整产品。

### 3. 它和你一开始做的版本相比，最大的变化是什么？
**参考答案：**  
最大的变化有两类。第一类是语义增强：tombstone、WAL rotation、更强的 sync、Raft metadata/log 持久化、snapshot 和 InstallSnapshot。第二类是运行时增强：从 embedded 单进程内嵌 5 节点，演进到 `cluster-node` 多进程真 5 节点，并且保留了 embedded 模式做兼容和确定性测试。

### 4. 现在这个项目最像什么？
**参考答案：**  
它最像一个“收敛过的分布式 KV / LSM / Raft 系统原型”。它不是 etcd、TiKV 或 RocksDB 的等价实现，但已经覆盖了这些系统里的很多核心问题缩影，比如 durability、幂等、一致性、failover、snapshot catch-up 和真实多进程 peer transport。

### 5. 这个项目最能体现你什么能力？
**参考答案：**  
我觉得最能体现三件事。第一是能从底层格式和协议把系统做起来，而不只是写上层业务。第二是能把“当前真实边界”讲清楚，不会乱吹。第三是工程闭环意识比较强，不只是实现功能，还会补测试、脚本、文档和演进路径。

### 6. 现在这个项目最重要的边界是什么？
**参考答案：**  
它已经支持同机多进程真 5 节点、snapshot 和 InstallSnapshot，但还不是生产级分布式数据库。它没有动态成员变更、没有 chunked snapshot 传输、没有跨机器部署治理、没有完整 observability、没有成熟的发布和运维体系。

### 7. 如果简历上只能写三点，你会怎么写？
**参考答案：**  
- 从零实现支持 embedded 与 `cluster-node` 双模式的 KVStore 原型，打通存储引擎、Raft 一致性、gRPC 服务与同机多进程真 5 节点集群。  
- 实现 `WAL / MemTable / SSTable / Compaction / Tombstone / Snapshot`，补齐 durability、恢复和 follower snapshot catch-up 语义。  
- 建立覆盖 deterministic、gRPC、chaos、benchmark 和多进程 failover 的验证闭环，确保改动不仅“能跑”，而且有证据可证明。

## 二、架构与运行模式

### 8. 当前系统的总体分层是什么？
**参考答案：**  
可以概括成：`client gRPC API -> service 编排层 -> raft 层 -> engine 层 -> on-disk state`。另外现在多了一层 `peer gRPC + transport abstraction`，用于节点间 Raft RPC。

### 9. 现在有哪些核心模块？
**参考答案：**  
核心模块包括：
- `api`：client-facing gRPC 适配
- `service`：请求校验、幂等、leader gate、snapshot 编排
- `raft`：选主、复制、commit、snapshot metadata、InstallSnapshot
- `engine`：WAL、MemTable、SST、Compaction、snapshot export/import
- `transport`：InProcessTransport / NetworkTransport
- `runtime`：embedded 和 cluster-node 的启动/配置路径

### 10. 为什么保留 embedded 和 cluster-node 双模式？
**参考答案：**  
因为它们解决的是不同问题。embedded 模式给我一个非常稳定、可控、确定性的测试和调试环境；cluster-node 模式则把系统推向真实多进程 peer 网络。保留双模式能让我在不牺牲稳定性的前提下逐步演进架构。

### 11. embedded 模式现在是什么？
**参考答案：**  
embedded 模式下，一个 `kvd` 进程里仍然内嵌 5 个逻辑节点，继续通过 in-process transport 互相通信。这个模式现在主要服务于 deterministic 测试、快速本地验证和协议级调试。

### 12. cluster-node 模式是什么？
**参考答案：**  
cluster-node 模式下，一个 `kvd` 进程只代表一个真实节点，它有一个本地 `RaftNode`、一个本地 `KvEngine`、一个 client listener 和一个 peer listener，通过 peer gRPC 和其他节点通信。

### 13. 为什么 cluster-node 下要拆 `client_addr` 和 `peer_addr`？
**参考答案：**  
因为 client API 和 peer RPC 的语义、流量和安全边界本来就不同。拆开以后职责更清晰，也更接近真实系统，后面如果要加 peer TLS、限流或者独立 metrics，也会更自然。

### 14. 为什么 peer RPC 单独做了 `raft.proto`，不复用 `kv.proto`？
**参考答案：**  
因为 client API 和节点间 Raft 协议不应该混在一个 contract 里。`kv.proto` 是外部 API，`raft.proto` 是内部节点间协议。分开之后 public wire contract 更稳定，内部实现也更容易演进。

### 15. transport abstraction 现在怎么拆的？
**参考答案：**  
现在有两类 transport：
- `InProcessTransport`：用于 embedded / deterministic 路径
- `NetworkTransport`：用于 cluster-node 多进程 peer gRPC 路径  
它们都围绕内部 `RaftNode` 的 message 结构工作，不让 `RaftNode` 自己依赖 gRPC 类型。

### 16. 为什么 `RaftNode` 不直接依赖 gRPC？
**参考答案：**  
因为一致性状态机和网络协议应该解耦。`RaftNode` 应该只理解 Raft 消息本身，不该知道 protobuf 或 gRPC。这样 embedded 测试和真实网络运行都能复用同一套协议逻辑。

### 17. `kvd` 现在做什么，不做什么？
**参考答案：**  
`kvd` 现在主要做参数解析、模式选择和 runtime 启动，不应该承载太多业务或协议逻辑。真正的状态管理和请求处理应该放在 service/runtime/raft 这些模块里。

### 18. 这个项目现在是多线程吗？
**参考答案：**  
是的。cluster-node 模式下至少有 gRPC server 线程和 Raft ticker 线程。再加上 peer RPC 和 client RPC 的处理路径，它已经不是单线程 demo 了。

### 19. 这个项目现在是“真集群”了吗？
**参考答案：**  
如果“真集群”的意思是“多进程节点通过真实网络地址互联”，那同机场景下可以说是的。但如果指跨机器、带完整运维和部署能力的分布式系统，那还不是。

### 20. 为什么先做 same-host 多进程，而不是直接跨机器？
**参考答案：**  
因为 same-host 多进程已经能把真正关键的问题暴露出来：peer RPC、端口管理、leader redirection、failover、catch-up、状态恢复。先把这些做稳，再往跨机器扩，比一开始把问题空间拉太大更合理。

## 三、写入链路、读取链路与服务语义

### 21. 现在一次 `Put` 的链路是什么？
**参考答案：**  
客户端请求进入 node 的 client listener，经由 `GrpcKvService` 到 service 层。service 先校验参数和幂等键，再判断当前节点是不是 leader。如果不是 leader，直接返回 `FAILED_PRECONDITION + leader_hint`；如果是 leader，则调用 `RaftNode::Propose`，通过 transport 把日志复制给 followers，多数派 commit 后由状态机 apply 到本地引擎，最后等到结果 future ready 才返回成功。

### 22. 一次 `Get` 的语义是什么？
**参考答案：**  
当前仍然是 leader-only linearizable read。也就是只允许 leader 读，并且 leader 还必须保持 quorum contact，否则直接拒绝。这是一个偏保守但很清晰的工程选择。

### 23. `Delete` 现在和早期版本相比最大的不同是什么？
**参考答案：**  
最大的不同是 delete 已经不只是“从 MemTable 里 erase 一下”。现在 delete 会形成真正的 tombstone 语义，能跨 flush、SST、compaction 和恢复继续生效。

### 24. 现在为什么 `Put/Delete` 要求 `request_id`？
**参考答案：**  
因为写请求的幂等是系统语义的一部分。它不仅在 service 层防止在线重试重复执行，也要在恢复、snapshot install 后继续保持一致的幂等行为。

### 25. 为什么相同 `request_id` 但不同内容要报错？
**参考答案：**  
因为幂等键代表的是“同一个逻辑操作”。如果同一个 `request_id` 对应不同 key/value，那系统不应该猜哪个才是真的，直接拒绝更安全。

### 26. 现在服务层为什么还要维护 inflight/completed 两张表？
**参考答案：**  
`inflight` 负责把并发重试合并到同一个结果 future，`completed` 负责让已成功请求快速返回，不重复 propose。这是 service 层的在线幂等优化；底层 WAL / snapshot 里的 request_id 则提供持久语义。

### 27. 现在多进程模式下 follower 收到 client 写请求会怎么处理？
**参考答案：**  
它会直接返回 not leader，不会偷偷代理给 leader。这个选择的好处是职责清晰，也更符合“客户端可连任意节点，但真正处理写请求的还是 leader”这个模型。

### 28. 为什么 `leader_hint` 目前还是 node id，不是完整地址？
**参考答案：**  
因为这是一个兼容旧 contract 的最小方案。node id 已经足够表达 redirect 意图，而 client_addr 可以由配置层映射。后续如果要做更友好的 external client 体验，再扩成结构化 leader address 也不迟。

## 四、存储引擎与 durability

### 29. 存储引擎当前有哪些核心能力？
**参考答案：**  
现在核心能力包括：
- WAL append + ordered replay
- MemTable with tombstone semantics
- SSTable read/write
- Compaction
- Block Cache
- checksum fail-closed
- snapshot export/import

### 30. 现在 WAL 和早期版本相比提升了什么？
**参考答案：**  
现在不再是单个无限增长的 WAL 文件。已经有 generation 发现和顺序 replay，也有 flush 之后切到新 WAL generation 的逻辑，同时 append 后会做更强的 sync，而不只是简单 flush。

### 31. 你现在还会说“没有 WAL rotation”吗？
**参考答案：**  
不会，因为这已经不是当前事实了。现在更准确的说法是：WAL generation 和 rotation 已经有了，但还不是完整的 manifest/version-set 管理体系。

### 32. 你现在还会说“没有 fsync”吗？
**参考答案：**  
也不会用那种绝对说法了。更准确的说法是：现在已经加入了更强的文件同步语义，但它仍然是一个工程原型级 durability 边界，而不是完整生产级 IO 策略。

### 33. tombstone 现在是怎么表示的？
**参考答案：**  
现在 tombstone 是系统内的一等语义。MemTable 里可以明确表示 tombstone；SST entry 也有 entry kind；compaction 时 tombstone 不会被忽略；读取时一旦命中 tombstone 会停止继续向旧 SST 回退。

### 34. 为什么 delete 不能只靠“新层没有这个 key”来表示？
**参考答案：**  
因为旧值可能还躺在更老的 SST 里。没有显式 tombstone，delete 就不能跨层遮蔽旧值，读路径会把历史值又读出来。

### 35. `KvEngine::Get` 现在怎么处理 tombstone？
**参考答案：**  
先查 MemTable，如果是 tombstone 就直接返回 not found，不继续向旧 SST 查。SST 读出来如果是 tombstone 也是同样逻辑。这个“命中 tombstone 就终止”的规则是 delete 正确性的关键。

### 36. snapshot payload 里为什么不需要保存 tombstone？
**参考答案：**  
因为 snapshot 代表的是某一时刻的完整逻辑状态，不是增量 patch。对于 snapshot 来说，不存在的 key 直接不存在即可，不需要再用 tombstone 去遮蔽更老层，因为安装 snapshot 本身就会替换旧状态。

### 37. snapshot payload 里为什么要带 request_id 集合？
**参考答案：**  
因为 snapshot 安装后系统仍然要保持幂等语义。如果不带 request_id 历史，snapshot install 之后重复请求可能会被重新执行，这会破坏当前对客户端承诺的语义。

### 38. 现在引擎 snapshot install 是怎么做的？
**参考答案：**  
它会解析 snapshot payload，重建完整的 live key/value 状态和 request_id 历史，然后清理当前 WAL/SST 状态，写入新的快照后基线状态，再重新打开当前活跃 WAL writer。重点是让安装后的逻辑状态与快照完全一致。

### 39. 为什么 snapshot export/import 不直接复用 SST 文件？
**参考答案：**  
因为 snapshot 语义比单个 SST 文件更高一层。SST 是 LSM 的一个持久化部件，而 snapshot 是完整状态机替换。把两者绑死会让后续演进更困难。

### 40. 现在存储层还缺什么？
**参考答案：**  
还缺更完整的 manifest/version-set、bloom filter、更成熟的后台 compaction/snapshot 调度、以及更细的 durability / IO 策略控制。

## 五、Raft、一致性与 snapshot

### 41. 现在 Raft 层除了选主和复制，还补了什么？
**参考答案：**  
现在除了选主、日志复制、commit、quorum gate 之外，还补了 durable metadata/log、snapshot metadata、InstallSnapshot、以及基于 snapshot base 的日志截断和 lagging follower catch-up。

### 42. 为什么 snapshot support 会强迫你改 log indexing？
**参考答案：**  
因为 snapshot 之后，逻辑日志的起点不再是 0 或 1，而是 `last_included_index`。这时如果还把“逻辑 index”直接当 vector 下标，就一定会错。所以要引入 `log_base_index` / `log_base_term` 这类概念。

### 43. 现在 `log_[i]` 还代表逻辑 index 吗？
**参考答案：**  
不再能直接这么理解。现在 `log_[0]` 更像 snapshot/base entry，逻辑 index 需要通过 base offset 去换算。

### 44. 为什么 `AdvanceCommitIndex()` 和 `ApplyCommitted()` 也要改成 base-index-aware？
**参考答案：**  
因为 snapshot 之后，不只是追加和读取会受影响，commit 判断和 apply 阶段如果还按旧下标访问，就会把错误的 entry 当成当前逻辑 index 对应的日志项。

### 45. 现在 snapshot metadata 持久化了什么？
**参考答案：**  
至少持久化了 `last_included_index` 和 `last_included_term`，同时 durable Raft storage 里还保留 `current_term`、`voted_for` 和 snapshot 之后的日志后缀。

### 46. `InstallSnapshot` 现在是什么形态？
**参考答案：**  
现在是一个 unary peer RPC。对当前 same-host 真集群和测试规模来说，这样已经够了；还没有做 chunked/streaming snapshot 传输。

### 47. Leader 在什么情况下会发 `InstallSnapshot`，而不是继续发 `AppendEntries`？
**参考答案：**  
当 follower 落后到已经在 leader 本地被 snapshot 截掉的前缀之前，也就是 follower 的 `next_index` 不再能用现有日志后缀追上时，leader 就要转而发送 snapshot。

### 48. follower 收到 `InstallSnapshot` 后做什么？
**参考答案：**  
它先安装 snapshot payload 到本地引擎，再让 `RaftNode` 更新 snapshot metadata 和截断本地日志前缀，最后返回成功响应给 leader。也就是说，状态机和 Raft 元数据两边都要同步推进。

### 49. 为什么 snapshot 创建没有直接放在 `RaftNode` 里做？
**参考答案：**  
因为 `RaftNode` 本身并不拥有状态机存储，只拥有协议状态。真正的 snapshot payload 生成必须依赖 `KvEngine`，所以 snapshot 创建更适合由 service/runtime 层协调。

### 50. 现在自动 snapshot 策略是什么？
**参考答案：**  
现在是一个基于提交进度和阈值的最小自动策略。不是生产级调度器，但已经能在 cluster-node 路径里自动触发 leader snapshot，并在 lagging follower 恢复时走 snapshot catch-up。

### 51. 你现在会说 snapshot 做完了吗？
**参考答案：**  
我会说“核心闭环已经做了”，包括 local snapshot、snapshot metadata、InstallSnapshot 和 follower catch-up；但更高级的东西，比如 chunked streaming、压缩、跨机器大快照优化、成熟的后台策略，还没有做完。

### 52. 为什么不直接做 chunked snapshot？
**参考答案：**  
因为当前 same-host 和当前数据规模下，先做 unary 全量 snapshot 更能快速证明语义闭环。chunked 传输是下一阶段优化，而不是第一步必须有的东西。

### 53. 现在 snapshot 最值钱的地方是什么？
**参考答案：**  
它把系统从“只能靠完整日志后缀追赶”提升到了“能在日志截断后继续恢复 lagging follower”。这对真正的集群运行来说是一个质变，不是单纯的性能优化。

## 六、多进程真集群与 peer networking

### 54. cluster-node 模式现在到底做到什么程度？
**参考答案：**  
已经能在同一台机器上起 5 个真实 `kvd --mode=cluster-node` 进程，每个节点有独立 client/peer 端口，能够完成 leader election、client redirect、写入复制、leader failover，以及 snapshot catch-up。

### 55. 为什么你说这比 embedded 有更高的工程价值？
**参考答案：**  
因为 embedded 主要证明协议语义，cluster-node 才开始真正暴露进程边界、peer RPC、端口、启动、日志、leader 重定向这些现实问题。对基础架构岗来说，这一步的信号价值很高。

### 56. `NetworkTransport` 为什么最后做成异步发送？
**参考答案：**  
因为同步 peer RPC 在持锁路径里很容易形成跨节点互等，尤其选举阶段多个 candidate 同时发票时更明显。异步发送虽然不是最终最优解，但能先把正确性和系统活性拉回来。

### 57. 你是怎么定位到 transport 需要异步化的？
**参考答案：**  
我不是拍脑袋改的，而是通过多进程集群测试和选主失败现象，发现节点间存在持锁同步等待的问题。这个症状非常像跨节点互等，所以最终把 peer RPC 发送从同步改成了异步回喂。

### 58. 为什么 `RaftPeerService` 要单独存在？
**参考答案：**  
因为 peer RPC 和 client API 的 handler 逻辑是完全不同的。单独拆出来之后，public API 和 internal protocol 不会互相污染，也更容易测。

### 59. 现在 launcher 脚本做了什么？
**参考答案：**  
本地 launcher 会根据静态配置启动 5 个 cluster-node 进程，写 pid/log 文件，并提供 stop 脚本做清理。它不是生产部署系统，但足够支撑本机集成测试和日常开发。

### 60. 为什么要有 sample config，而不是每次手敲参数？
**参考答案：**  
因为 5 节点真集群里节点列表、端口和 data_dir 这些信息会被多个模块复用。配置文件比散落在命令行里的参数更稳定，也更容易测试和复现。

### 61. 现在多进程测试主要证明什么？
**参考答案：**  
主要证明三件事：  
1. peer networking 真的通了  
2. follower 重定向和 leader 写入语义在真实多进程下也成立  
3. lagging follower 能通过 snapshot catch-up 回来

### 62. 你会把 same-host 多进程说成“生产分布式系统”吗？
**参考答案：**  
不会。我会说这是“真多进程本地集群”，它已经跨过了 embedded 原型的边界，但还没到完整生产分布式系统的层级。

## 七、gRPC、接口与错误语义

### 63. 公共 client API 现在变了吗？
**参考答案：**  
没变。对外还是 `Put/Get/Delete` 三个 unary RPC，`kv.proto` 没有扩展成事务、watch、lease 之类的东西。peer RPC 的新增在 `raft.proto`，不属于 public client contract。

### 64. 为什么 public API 不变，但内部多了 `raft.proto`？
**参考答案：**  
因为 external contract 稳定和 internal evolution 是两件事。public API 继续稳定，对客户端最友好；内部 peer 协议扩展是实现层的事情。

### 65. `GrpcKvService` 现在还能复用吗？
**参考答案：**  
能，因为我把它依赖的对象从具体 `KvRaftService` 抽到了更通用的 `KvService` 接口。这样 embedded 和 cluster-node 两种 runtime 都能共用同一套 client-facing gRPC adapter。

### 66. 现在 cluster-node 模式下的 not-leader 行为是什么？
**参考答案：**  
仍然是返回 `FAILED_PRECONDITION`，并在 message 里带 `leader_hint`。也就是说，即便到了真多进程模式，client-facing 语义仍然尽量保持兼容。

### 67. 现在 peer RPC 也走 TLS 吗？
**参考答案：**  
目前没有。client-facing TLS 还在，但 peer RPC 这层现在主要解决的是真正的多进程协议路径，还没有做 peer TLS。

## 八、测试、验证与工程化

### 68. 现在测试分层是什么？
**参考答案：**  
现在至少可以分成四层：
- deterministic / embedded 协议测试
- engine / recovery / integrity 测试
- gRPC / service 语义测试
- same-host 多进程 cluster-node 测试  
而且 snapshot 现在已经同时覆盖 deterministic 和多进程两条路径。

### 69. 新增的 `snapshot_test` 证明什么？
**参考答案：**  
它证明两件事：  
1. engine snapshot export/import 正确  
2. embedded 路径里 lagging follower 能通过 InstallSnapshot catch-up

### 70. `multi_process_cluster_test` 现在证明什么？
**参考答案：**  
它现在不只是 failover 了，还证明：
- 真 5 节点进程能启动
- leader election 和 redirect 成立
- leader failover 成立
- lagging follower 在多进程模式下能通过 snapshot catch-up

### 71. 你现在最看重哪条验证？
**参考答案：**  
我最看重 `multi_process_cluster_test` 和 `snapshot_test`。前者证明系统已经跨过 embedded 边界，后者证明 snapshot 这条很容易写错的语义链路真的闭上了。

### 72. 你为什么一直强调“有 fresh verification evidence 再说完成”？
**参考答案：**  
因为基础架构系统最怕“我觉得应该行”。尤其是 durability、failover、snapshot 这种东西，如果不看 fresh test output，就很容易在错误的状态上继续叙事。

### 73. 现在还有哪些门禁没完全跟上？
**参考答案：**  
仓库层面还有一个和本地 WSL worktree 元数据有关的 `release readiness` 兼容问题，不是 cluster/snapshot 逻辑问题。代码和测试已经通了，但 release 脚本的 WSL worktree 兼容还值得单独修。

## 九、现在仍然没做的东西

### 74. 现在还没做动态成员变更，对吗？
**参考答案：**  
对，还是静态 5 节点配置。这次重点是把真集群、durability、snapshot 和 catch-up 路径做对，不是同时把 membership change 也拉进来。

### 75. chunked snapshot 做了吗？
**参考答案：**  
还没有。当前是 unary 全量 snapshot payload，足够支撑 same-host 本地集群和当前测试规模。

### 76. peer TLS 做了吗？
**参考答案：**  
还没有。现在 client-facing TLS 在，peer RPC 暂时还是明文。

### 77. observability 做到什么程度？
**参考答案：**  
还没有系统化的 metrics / tracing / structured logging 方案。现在主要是靠测试、返回值、launcher 和文件级持久化状态来验证行为。

### 78. 这个项目现在还是“原型”吗？
**参考答案：**  
是，但已经是一个工程深度明显更高的原型。它不再只是单机/单进程逻辑演示，而是具备了真实多进程 cluster、snapshot 和 catch-up 的系统原型。

## 十、刁钻追问

### 79. 如果面试官说“你这也不算完成 snapshot，只是最低配”，你怎么答？
**参考答案：**  
我会认同一半。是的，现在是最低配但完整闭环的 snapshot，而不是成熟产品方案。但它已经覆盖了最关键的语义：local snapshot、log truncation、InstallSnapshot、follower catch-up。后续优化方向我也很清楚，比如 chunking、压缩和更成熟的后台调度。

### 80. 如果面试官问“你为什么不一步到位做 chunked snapshot + 跨机器？”
**参考答案：**  
因为我优先追求正确性闭环，而不是一开始把问题空间拉满。只要 `InstallSnapshot` 和 catch-up 这条主语义没做稳，先堆 chunking 和跨机器只会让定位和验证更难。

### 81. 如果面试官问“现在最值得继续做什么？”
**参考答案：**  
我会优先做三件事：  
1. peer TLS  
2. chunked snapshot  
3. release / worktree / ops 兼容性和 observability  
这三件事最能把当前原型继续往“可长期维护的系统”推进。

### 82. 如果面试官让你总结“这版项目最值钱的变化”？
**参考答案：**  
我会说最值钱的变化不是多写了几个文件，而是它从“协议和存储都在一个进程里验证”推进到了“多进程真节点 + snapshot catch-up + durable state + 完整测试闭环”。这意味着它已经从一个强原型走向一个更真实的系统雏形。

## 使用建议

- 如果你只背最关键的题，先背：`1 / 10 / 29 / 42 / 54 / 69 / 79 / 82`
- 面试时先讲当前状态，再补“早期是 embedded，后来逐步演进到这里”
- 不要再用旧说法描述当前项目，比如：
  - “没有 tombstone”
  - “没有 WAL rotation”
  - “没有 durable Raft metadata/log”
  - “没有 snapshot”
  - “只有单进程 embedded”
- 遇到“现在还差什么”时，直接说真实边界，而不是为了显得强行什么都做过
