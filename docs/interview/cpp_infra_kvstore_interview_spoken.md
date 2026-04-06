# KVStore 面试口语稿（当前版）

> 这份稿子不是完整题库，而是更适合真实面试直接说出口的版本。
> 内容按 `kvstore-hardening` 分支当前状态整理，重点讲现在已经做出来的能力，而不是早期版本。

## 1. 1 分钟项目介绍

### 面试官：你介绍一下这个项目。
**口语答法：**  
这个项目本质上是我自己持续演进的一个 C++ KVStore 原型。最开始它只是一个单进程内嵌 5 节点的 Raft 原型，后来我不断把它往更真实的系统方向推。现在底层已经有 WAL、MemTable、SSTable、Compaction、tombstone、WAL rotation 和更强的 sync 语义；Raft 这边做了 metadata 和 log 持久化、snapshot 和 InstallSnapshot；运行时也从早期的 embedded 模式，扩到了 `cluster-node` 多进程真 5 节点模式。  
我觉得这个项目最能体现的是，我不只是把功能做出来，而是会把 durability、一致性、故障切换和验证链路一起补完整。

### 面试官：这个项目现在最准确的定义是什么？
**口语答法：**  
我会把它定义成一个支持 embedded 模式和同机多进程 cluster-node 模式的 KVStore 原型。它已经有比较完整的系统语义，但我不会把它讲成生产级数据库。

## 2. 为什么值得讲

### 面试官：为什么你觉得这个项目有价值？
**口语答法：**  
因为它覆盖的不是单一技术点，而是一整条基础架构链路。往下有存储格式、WAL、SST、snapshot，往上有 Raft、一致性、failover，再往外有 gRPC 和多进程节点运行时。对 C++ 基础架构岗来说，这种项目比单点 demo 更能体现工程能力。

### 面试官：你在这个项目里最核心的贡献是什么？
**口语答法：**  
我最核心的贡献是把系统从“逻辑上能跑”一步步推进到“语义更完整，而且能证明它对”。比如 tombstone、WAL rotation、Raft 持久化、snapshot、cluster-node、多进程 failover 和测试闭环，这些其实都不是孤立功能，而是围绕“这个系统在故障下到底还能不能说得清楚”去做的。

## 3. 架构怎么讲

### 面试官：整体架构你怎么概括？
**口语答法：**  
我一般会说成几层：最外面是 client-facing gRPC API，中间是 service 层做校验、幂等和 leader gate，再往下是 Raft，一致性和复制在这里处理，最底下是引擎层，也就是 WAL、MemTable、SST、Compaction 和 snapshot export/import。  
另外现在还多了一层 peer gRPC 和 transport abstraction，因为节点之间已经不是进程内对象通信了。

### 面试官：现在为什么会有 embedded 和 cluster-node 两种模式？
**口语答法：**  
因为它们解决的是两个不同问题。embedded 模式特别适合做确定性测试和快速调试；cluster-node 模式则是为了把系统推进到真实多进程、真实 peer networking 的形态。保留双模式的好处是，我不用为了追求真实运行时，把原来很稳定的测试环境全部牺牲掉。

### 面试官：cluster-node 模式是什么样？
**口语答法：**  
cluster-node 模式下，一个 `kvd` 进程只代表一个真实节点。它有自己的本地 `RaftNode`、本地 `KvEngine`、一个 client listener，还有一个 peer listener。client listener 对外提供 `Put/Get/Delete`，peer listener 只处理节点间的 Raft RPC。

### 面试官：为什么 client 和 peer 端口要分开？
**口语答法：**  
因为这两个流量的角色不一样。client 是外部 API，peer 是内部协议。把它们拆开以后，职责清楚很多，后面不管是做 TLS、限流还是可观测性，边界都会更干净。

## 4. 存储引擎怎么讲

### 面试官：存储引擎的核心设计是什么？
**口语答法：**  
存储引擎这一层我用了比较典型的 LSM 风格思路。写入先落 WAL，再进 MemTable，之后可以 flush 成 SST，再通过 compaction 合并。  
和最早版本相比，现在更关键的是我把 tombstone、WAL generation、stronger sync 和 snapshot export/import 这些语义补进去了，所以它不再只是“有文件落盘”，而是能更清楚地回答 durability 和恢复问题。

### 面试官：为什么一定要先写 WAL 再改内存？
**口语答法：**  
因为 WAL 是恢复依据。如果先改内存再写 WAL，中间崩了，这次写入就没法重建。先 WAL 后 MemTable 是最基本的恢复语义。

### 面试官：现在 WAL 和以前相比有什么升级？
**口语答法：**  
现在不再是单个无限增长的 WAL 文件了。我加了 generation 发现和顺序 replay，也有 flush 之后切换到新 WAL generation 的逻辑。另外 append 后不只是简单 flush，还补了更强的 sync 语义。

### 面试官：delete 现在为什么不再是个坑了？
**口语答法：**  
因为现在 delete 已经是显式 tombstone 语义了。以前最大的坑是，老 SST 里的值可能会在 delete 之后又被读出来。现在 tombstone 会跨 MemTable、SST、compaction 和恢复一起生效，一旦命中 tombstone，读取就不会继续往旧层回退。

### 面试官：snapshot payload 里为什么还要带 request_id？
**口语答法：**  
因为 snapshot 安装完以后，系统还是要保持写请求幂等。如果不把 request_id 历史也一起带过去，那 snapshot 之后重复请求可能会被重新执行，这会破坏之前已经对客户端承诺的语义。

## 5. durability 和恢复怎么讲

### 面试官：你现在会怎么描述 durability？
**口语答法：**  
我会说，现在 durability 已经比最开始强很多了，但我还是不会把它讲成完整生产级 durability。更准确的表述是：系统已经有 WAL、rotation、更强 sync、恢复和快照语义，能够比较严肃地讨论 acknowledged write loss、崩溃恢复和 follower catch-up，但它还不是成熟的存储产品。

### 面试官：你怎么理解“ack 到底意味着什么”？
**口语答法：**  
这是我在这个项目里学到最重要的点之一。ack 不能只是“代码执行到某一步了”，而应该对应一个你能解释清楚的语义边界。比如是 propose 成功，还是 majority commit，还是状态机 apply 完成，还是 WAL 持久化完成。这个项目里，我一直在逼自己把这个边界说清楚，而不是只看“功能过了”。

## 6. Raft 怎么讲

### 面试官：Raft 这部分你现在做到什么程度？
**口语答法：**  
现在除了最基本的选主、日志复制、majority commit、failover 之外，我还做了 Raft metadata 和 log 的持久化，以及 snapshot 和 InstallSnapshot。  
也就是说，它已经不只是一个内存态的协议原型了，而是开始具备真正的 restart 语义和 lagging follower catch-up 语义。

### 面试官：为什么 snapshot 会强迫你改动很多索引逻辑？
**口语答法：**  
因为 snapshot 之后，日志的逻辑起点变了。你不能再默认“逻辑 index 就等于 vector 下标”。所以必须引入 snapshot base index 和 base term 这类概念，再把读取、复制、commit、apply 都改成基于逻辑索引换算。

### 面试官：现在 leader 在什么情况下会发 InstallSnapshot？
**口语答法：**  
当 follower 落后到 leader 已经 snapshot 并截断掉的那段日志之前，就不能靠 AppendEntries 补了，这时候 leader 就要转成发送 InstallSnapshot。

### 面试官：follower 收到 InstallSnapshot 之后做什么？
**口语答法：**  
它会先把 snapshot payload 装进本地引擎，再更新本地 Raft snapshot metadata 和日志后缀。也就是说，状态机和 Raft 协议状态要一起推进，不能只改一边。

### 面试官：snapshot 现在算做完了吗？
**口语答法：**  
如果说的是核心闭环，那已经做了：local snapshot、snapshot metadata、InstallSnapshot、follower catch-up 都有了。  
但如果说的是成熟产品级 snapshot，那还远没做完，比如 chunked 传输、压缩、更聪明的后台调度，这些都还是后续方向。

## 7. 多进程真集群怎么讲

### 面试官：现在这个项目已经是“真集群”了吗？
**口语答法：**  
如果真集群的定义是“多个真实进程通过真实网络地址互联”，那同机场景下是的。  
但如果面试官的意思是跨机器部署、带成熟运维体系的分布式系统，那我不会说是。

### 面试官：peer RPC 为什么单独做成 `raft.proto`？
**口语答法：**  
因为 client API 和节点间 Raft 协议不应该混在一起。`kv.proto` 是 public contract，`raft.proto` 是 internal peer contract。分开之后，public API 可以保持稳定，内部协议也更容易演进。

### 面试官：为什么 `NetworkTransport` 最后做成异步？
**口语答法：**  
因为同步 peer RPC 在持锁路径里很容易把系统卡住，尤其是多个节点同时在选主或者互相等待响应的时候。异步化之后，至少先把活性问题解决了，再继续往更成熟的线程模型演进。

### 面试官：你是怎么验证 cluster-node 真的通了？
**口语答法：**  
我不是只看单节点启动成功，而是补了同机 5 节点 launcher 和真实多进程集成测试。测试会去验证 leader election、client redirect、leader failover，还有后面加上的 snapshot catch-up。  
也就是说，我验证的是“系统行为”，不是“进程起来了”。

## 8. 测试和证据怎么讲

### 面试官：你现在最关键的测试有哪些？
**口语答法：**  
如果只挑最值钱的几条，我会说：
- `snapshot_test`
- `multi_process_cluster_test`
- `bench_gate_test`
- 再加上原来的 deterministic Raft 和 gRPC integration 测试  
因为这几条能直接证明 durability、一致性、多进程 failover 和 snapshot catch-up 这些最难说清楚的语义。

### 面试官：`snapshot_test` 证明什么？
**口语答法：**  
它证明两件事：一是 engine snapshot export/import 的语义是对的；二是 embedded 路径里，lagging follower 能通过 InstallSnapshot catch-up。

### 面试官：`multi_process_cluster_test` 现在证明什么？
**口语答法：**  
它现在不只是简单 failover 了，还证明：
- 真 5 节点 cluster-node 能启动
- follower 写会被拒绝并返回 leader hint
- leader failover 成立
- lagging follower 在多进程模式下也能通过 snapshot catch-up 回来

### 面试官：为什么你老强调“证据”？
**口语答法：**  
因为这种系统最怕的是“我觉得应该行”。特别是 durability、snapshot、failover 这种东西，如果没有 fresh test output，很容易自己骗自己。这个项目里，我一直尽量让“我说它能做什么”后面都有对应验证。

## 9. 现在还没做的东西

### 面试官：现在最明显还没做的是什么？
**口语答法：**  
我会直接说：
- 没有 dynamic membership
- 没有 peer TLS
- 没有 chunked snapshot
- 没有成熟的 observability
- 没有真正的跨机器部署治理  
这些都不是“不知道怎么做”，而是我有意识地控制了这轮范围。

### 面试官：为什么不继续把 peer TLS 也一起做了？
**口语答法：**  
因为我这轮优先解决的是更核心的系统语义问题，也就是 durability、真实多进程和 snapshot catch-up。peer TLS 当然重要，但它不应该抢在这些核心路径之前。

### 面试官：为什么你不直接把它讲成生产级数据库？
**口语答法：**  
因为那样不诚实。我觉得真正有说服力的方式不是吹大，而是把“现在已经做到了什么”和“离生产级还差什么”都说清楚。这样反而更像一个靠谱的工程师。

## 10. 高频结尾答法

### 面试官：你觉得这版项目最值钱的变化是什么？
**口语答法：**  
我会说，最值钱的变化不是单个功能点，而是系统整体形态变了。它已经从一个 embedded 协议原型，推进到了一个有真实多进程节点、durable state、snapshot 和 catch-up 语义、而且有测试闭环支撑的系统原型。这一步对我来说特别有价值。

### 面试官：如果让你继续做，你下一步会做什么？
**口语答法：**  
我会优先做三个方向：
1. peer TLS  
2. chunked snapshot  
3. release / worktree / ops 这层兼容性和 observability  
因为现在核心语义已经有了，下一步更应该把它往更稳、更可维护的方向推。

## 使用建议

- 先背：`1 / 10 / 29 / 42 / 54 / 68 / 80 / 82`
- 每个答案都尽量用你自己的话再顺一遍，别一字一句硬背
- 真实面试里，先讲主干，再根据追问往下钻
- 如果被问到“以前是不是没做这些”，可以直接说：
  - 早期确实没有
  - 后来我是怎么一步步补上的
  - 现在还剩哪些真实边界没做
