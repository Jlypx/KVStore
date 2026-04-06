#include "kvstore/service/kv_raft_service.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "kvstore/engine/kv_engine.h"
#include "kvstore/integrity/crc32c.h"
#include "kvstore/raft/cluster_runtime.h"
#include "kvstore/raft/raft_node.h"

namespace kvstore::service {
namespace {

constexpr std::size_t kMaxKeyBytes = 1024;
constexpr std::size_t kMaxValueBytes = 1024 * 1024;

enum class Op : std::uint8_t {
  kPut = 1,
  kDelete = 2,
};

struct Command {
  Op op = Op::kPut;
  std::string key;
  std::string value;
  std::string request_id;
};

auto WriteU32(std::string* out, std::uint32_t v) -> void {
  const char b[4] = {
      static_cast<char>((v >> 24) & 0xFF),
      static_cast<char>((v >> 16) & 0xFF),
      static_cast<char>((v >> 8) & 0xFF),
      static_cast<char>(v & 0xFF),
  };
  out->append(b, 4);
}

auto ReadU32(std::string_view in, std::size_t* offset, std::uint32_t* out)
    -> bool {
  if (*offset + 4 > in.size()) {
    return false;
  }
  const unsigned char* p =
      reinterpret_cast<const unsigned char*>(in.data() + *offset);
  *out = (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) |
         (static_cast<std::uint32_t>(p[3]));
  *offset += 4;
  return true;
}

auto EncodeCommand(const Command& cmd) -> std::string {
  // Versioned binary encoding for Raft log commands.
  //
  // [u8 version=1][u8 op]
  // [u32 request_id_len][bytes request_id]
  // [u32 key_len][bytes key]
  // [u32 value_len][bytes value]
  std::string out;
  out.reserve(2 + 12 + cmd.request_id.size() + cmd.key.size() + cmd.value.size());
  out.push_back(static_cast<char>(1));
  out.push_back(static_cast<char>(cmd.op));
  WriteU32(&out, static_cast<std::uint32_t>(cmd.request_id.size()));
  out.append(cmd.request_id);
  WriteU32(&out, static_cast<std::uint32_t>(cmd.key.size()));
  out.append(cmd.key);
  WriteU32(&out, static_cast<std::uint32_t>(cmd.value.size()));
  out.append(cmd.value);
  return out;
}

auto DecodeCommand(std::string_view bytes) -> std::optional<Command> {
  if (bytes.size() < 2) {
    return std::nullopt;
  }
  const auto version = static_cast<std::uint8_t>(bytes[0]);
  const auto op = static_cast<std::uint8_t>(bytes[1]);
  if (version != 1) {
    return std::nullopt;
  }
  Command cmd;
  if (op == static_cast<std::uint8_t>(Op::kPut)) {
    cmd.op = Op::kPut;
  } else if (op == static_cast<std::uint8_t>(Op::kDelete)) {
    cmd.op = Op::kDelete;
  } else {
    return std::nullopt;
  }

  std::size_t off = 2;
  std::uint32_t rid_len = 0;
  std::uint32_t key_len = 0;
  std::uint32_t value_len = 0;
  if (!ReadU32(bytes, &off, &rid_len)) {
    return std::nullopt;
  }

  if (rid_len > 4U * 1024U || off + rid_len > bytes.size()) {
    return std::nullopt;
  }
  cmd.request_id.assign(bytes.substr(off, rid_len));
  off += rid_len;

  if (!ReadU32(bytes, &off, &key_len)) {
    return std::nullopt;
  }
  if (key_len > kMaxKeyBytes || off + key_len > bytes.size()) {
    return std::nullopt;
  }
  cmd.key.assign(bytes.substr(off, key_len));
  off += key_len;

  if (!ReadU32(bytes, &off, &value_len)) {
    return std::nullopt;
  }
  if (value_len > kMaxValueBytes || off + value_len > bytes.size()) {
    return std::nullopt;
  }
  cmd.value.assign(bytes.substr(off, value_len));
  off += value_len;

  return off == bytes.size() ? std::optional<Command>(cmd) : std::nullopt;
}

auto MakeInvalid(const std::string& message) -> Error {
  return Error{.code = ErrorCode::kInvalidArgument, .message = message};
}

auto MakeUnavailable(const std::string& message) -> Error {
  return Error{.code = ErrorCode::kUnavailable, .message = message};
}

auto MakeNotLeader(kvstore::raft::NodeId hint) -> Error {
  Error e;
  e.code = ErrorCode::kNotLeader;
  e.message = "not leader";
  e.leader_hint = hint;
  return e;
}

}  // namespace

struct KvRaftService::Impl {
  explicit Impl(std::filesystem::path data_dir, RaftOptions options)
      : data_dir_(std::move(data_dir)), options_(std::move(options)) {}

  bool initialized = false;

  std::filesystem::path data_dir_;
  RaftOptions options_;

  mutable std::mutex mu;
  std::unique_ptr<kvstore::raft::RaftCluster> cluster;

  struct NodeState {
    std::unique_ptr<kvstore::engine::KvEngine> engine;
  };
  std::unordered_map<kvstore::raft::NodeId, NodeState> nodes;

  struct InflightPut {
    std::shared_ptr<std::promise<PutResult>> promise;
    std::shared_future<PutResult> future;
    std::uint32_t key_crc = 0;
    std::uint32_t value_crc = 0;
    std::size_t key_size = 0;
    std::size_t value_size = 0;
  };

  struct InflightDelete {
    std::shared_ptr<std::promise<DeleteResult>> promise;
    std::shared_future<DeleteResult> future;
  };

  struct PutCacheEntry {
    PutResult result;
    std::uint32_t key_crc = 0;
    std::uint32_t value_crc = 0;
    std::size_t key_size = 0;
    std::size_t value_size = 0;
  };
  std::unordered_map<std::string, PutCacheEntry> completed_puts;
  std::unordered_map<std::string, DeleteResult> completed_deletes;
  std::unordered_map<std::string, InflightPut> inflight_puts;
  std::unordered_map<std::string, InflightDelete> inflight_deletes;

  std::atomic<bool> stop{false};
  std::thread ticker;

  auto Init() -> bool {
    std::error_code ec;
    std::filesystem::create_directories(data_dir_, ec);
    if (ec) {
      return false;
    }

    kvstore::raft::EmbeddedClusterOptions ro;
    ro.node_ids = options_.node_ids;
    ro.election_timeout_min_ticks = options_.election_timeout_min_ticks;
    ro.election_timeout_max_ticks = options_.election_timeout_max_ticks;
    ro.heartbeat_interval_ticks = options_.heartbeat_interval_ticks;
    ro.quorum_timeout_ticks = options_.quorum_timeout_ticks;
    ro.storage_root = data_dir_;

    cluster = kvstore::raft::CreateEmbeddedRaftCluster(ro);

    for (kvstore::raft::NodeId id : options_.node_ids) {
      auto node_dir = data_dir_ / ("node" + std::to_string(id));
      std::filesystem::create_directories(node_dir, ec);
      if (ec) {
        return false;
      }
      auto wal = node_dir / "000001.wal";
      auto engine = std::make_unique<kvstore::engine::KvEngine>(wal);
      if (!engine->Open()) {
        return false;
      }
      nodes[id] = NodeState{.engine = std::move(engine)};

      auto* raft_node = cluster->node(id);
      if (raft_node == nullptr) {
        return false;
      }

      raft_node->SetOnCommitted(
          [this, id](std::vector<kvstore::raft::CommittedEntry> entries) {
            this->OnCommitted(id, std::move(entries));
          });
    }

    // Elect a leader and ensure quorum-contact gate is warm before serving.
    const auto leader = cluster->WaitForLeader(500);
    if (!leader.has_value()) {
      return false;
    }

    bool ready = false;
    for (std::uint64_t i = 0; i < 500; ++i) {
      cluster->Tick();
      const auto elected = cluster->FindLeader();
      if (!elected.has_value()) {
        continue;
      }
      const auto* leader_node = cluster->node(*elected);
      if (leader_node != nullptr &&
          leader_node->role() == kvstore::raft::Role::kLeader &&
          leader_node->HasQuorumContact()) {
        ready = true;
        break;
      }
    }
    if (!ready) {
      return false;
    }

    ticker = std::thread([this] { this->TickLoop(); });
    initialized = true;
    return true;
  }

  auto Shutdown() -> void {
    stop.store(true);
    if (ticker.joinable()) {
      ticker.join();
    }
  }

  auto TickLoop() -> void {
    while (!stop.load()) {
      {
        std::lock_guard<std::mutex> lock(mu);
        if (cluster) {
          cluster->Tick();
        }
      }
      std::this_thread::sleep_for(options_.tick_period);
    }
  }

  auto OnCommitted(kvstore::raft::NodeId node_id,
                   std::vector<kvstore::raft::CommittedEntry> entries) -> void {
    // Called on ticker thread while holding mu (due to cluster.Tick()).
    auto it = nodes.find(node_id);
    if (it == nodes.end() || !it->second.engine) {
      return;
    }

    for (const auto& entry : entries) {
      const auto decoded = DecodeCommand(entry.command);
      if (!decoded.has_value()) {
        continue;
      }
      const auto& cmd = decoded.value();

      if (cmd.op == Op::kPut) {
        const bool existed = it->second.engine->Get(cmd.key).has_value();
        const auto ar = it->second.engine->Put(cmd.key, cmd.value, cmd.request_id);
        if (!ar.Ok()) {
          continue;
        }
        PutResult res{.overwritten = existed};
        PutCacheEntry entry;
        entry.result = res;
        entry.key_crc = kvstore::integrity::ComputeCrc32c(cmd.key);
        entry.value_crc = kvstore::integrity::ComputeCrc32c(cmd.value);
        entry.key_size = cmd.key.size();
        entry.value_size = cmd.value.size();
        completed_puts[cmd.request_id] = entry;

        auto inflight = inflight_puts.find(cmd.request_id);
        if (inflight != inflight_puts.end() && inflight->second.promise) {
          inflight->second.promise->set_value(res);
          inflight_puts.erase(inflight);
        }
      } else if (cmd.op == Op::kDelete) {
        const bool existed = it->second.engine->Get(cmd.key).has_value();
        const auto ar = it->second.engine->Delete(cmd.key, cmd.request_id);
        if (!ar.Ok()) {
          continue;
        }
        DeleteResult res{.deleted = existed};
        completed_deletes[cmd.request_id] = res;

        auto inflight = inflight_deletes.find(cmd.request_id);
        if (inflight != inflight_deletes.end() && inflight->second.promise) {
          inflight->second.promise->set_value(res);
          inflight_deletes.erase(inflight);
        }
      }
    }
  }

  auto LeaderNodeLocked() -> std::pair<kvstore::raft::NodeId, kvstore::raft::RaftNode*> {
    if (!cluster) {
      return {kvstore::raft::kNoLeader, nullptr};
    }
    const auto leader_id = cluster->FindLeader();
    if (!leader_id.has_value()) {
      return {kvstore::raft::kNoLeader, nullptr};
    }
    return {*leader_id, cluster->node(*leader_id)};
  }
};

KvRaftService::KvRaftService(std::filesystem::path data_dir, RaftOptions options)
    : impl_(new Impl(std::move(data_dir), std::move(options))) {
  if (!impl_->Init()) {
    // Best-effort shutdown.
    impl_->Shutdown();
  }
}

KvRaftService::~KvRaftService() {
  if (impl_ != nullptr) {
    impl_->Shutdown();
    delete impl_;
    impl_ = nullptr;
  }
}

auto KvRaftService::Put(const std::string& key,
                        const std::string& value,
                        const std::string& request_id,
                        std::chrono::steady_clock::time_point deadline)
    -> Result<PutResult> {
  if (!impl_->initialized) {
    return MakeUnavailable("service not initialized");
  }
  if (key.size() > kMaxKeyBytes) {
    return MakeInvalid("key exceeds 1KiB limit");
  }
  if (value.size() > kMaxValueBytes) {
    return MakeInvalid("value exceeds 1MiB limit");
  }
  if (request_id.empty()) {
    return MakeInvalid("request_id must be non-empty");
  }
  if (request_id.size() > 4U * 1024U) {
    return MakeInvalid("request_id exceeds 4KiB limit");
  }

  std::shared_future<PutResult> wait;
  kvstore::raft::NodeId leader_hint = kvstore::raft::kNoLeader;
  kvstore::raft::ProposeResult propose;
  const auto key_crc = kvstore::integrity::ComputeCrc32c(key);
  const auto value_crc = kvstore::integrity::ComputeCrc32c(value);

  {
    std::lock_guard<std::mutex> lock(impl_->mu);

    const auto done = impl_->completed_puts.find(request_id);
    if (done != impl_->completed_puts.end()) {
      if (done->second.key_crc != key_crc || done->second.value_crc != value_crc ||
          done->second.key_size != key.size() ||
          done->second.value_size != value.size()) {
        return MakeInvalid("request_id reused with different key/value");
      }
      return done->second.result;
    }

    const auto inflight = impl_->inflight_puts.find(request_id);
    if (inflight != impl_->inflight_puts.end()) {
      if (inflight->second.key_crc != key_crc ||
          inflight->second.value_crc != value_crc ||
          inflight->second.key_size != key.size() ||
          inflight->second.value_size != value.size()) {
        return MakeInvalid("request_id reused with different key/value");
      }
      wait = inflight->second.future;
    } else {
      auto p = std::make_shared<std::promise<PutResult>>();
      wait = p->get_future().share();
      impl_->inflight_puts[request_id] = Impl::InflightPut{.promise = p,
                                                           .future = wait,
                                                           .key_crc = key_crc,
                                                           .value_crc = value_crc,
                                                           .key_size = key.size(),
                                                           .value_size = value.size()};

      auto [leader_id, leader_node] = impl_->LeaderNodeLocked();
      leader_hint = leader_id;
      if (leader_node == nullptr) {
        impl_->inflight_puts.erase(request_id);
        return MakeUnavailable("no leader elected");
      }
      propose = leader_node->Propose(EncodeCommand(Command{.op = Op::kPut,
                                                          .key = key,
                                                          .value = value,
                                                          .request_id = request_id}));
      leader_hint = propose.leader_hint;

      if (!propose.Ok()) {
        impl_->inflight_puts.erase(request_id);
        if (propose.status == kvstore::raft::ProposeStatus::kNotLeader) {
          return MakeNotLeader(propose.leader_hint);
        }
        if (propose.status == kvstore::raft::ProposeStatus::kQuorumUnavailable) {
          return MakeUnavailable("quorum unavailable");
        }
        return Error{.code = ErrorCode::kInternal, .message = "propose failed"};
      }
    }
  }

  const auto status = wait.wait_until(deadline);
  if (status != std::future_status::ready) {
    return Error{.code = ErrorCode::kTimeout,
                 .message = "timed out waiting for commit",
                 .leader_hint = leader_hint};
  }
  return wait.get();
}

auto KvRaftService::Delete(const std::string& key,
                           const std::string& request_id,
                           std::chrono::steady_clock::time_point deadline)
    -> Result<DeleteResult> {
  if (!impl_->initialized) {
    return MakeUnavailable("service not initialized");
  }
  if (key.size() > kMaxKeyBytes) {
    return MakeInvalid("key exceeds 1KiB limit");
  }
  if (request_id.empty()) {
    return MakeInvalid("request_id must be non-empty");
  }

  std::shared_future<DeleteResult> wait;
  kvstore::raft::NodeId leader_hint = kvstore::raft::kNoLeader;
  kvstore::raft::ProposeResult propose;

  {
    std::lock_guard<std::mutex> lock(impl_->mu);

    const auto done = impl_->completed_deletes.find(request_id);
    if (done != impl_->completed_deletes.end()) {
      return done->second;
    }

    const auto inflight = impl_->inflight_deletes.find(request_id);
    if (inflight != impl_->inflight_deletes.end()) {
      wait = inflight->second.future;
    } else {
      auto p = std::make_shared<std::promise<DeleteResult>>();
      wait = p->get_future().share();
      impl_->inflight_deletes[request_id] =
          Impl::InflightDelete{.promise = p, .future = wait};

      auto [leader_id, leader_node] = impl_->LeaderNodeLocked();
      leader_hint = leader_id;
      if (leader_node == nullptr) {
        impl_->inflight_deletes.erase(request_id);
        return MakeUnavailable("no leader elected");
      }
      propose = leader_node->Propose(EncodeCommand(Command{.op = Op::kDelete,
                                                          .key = key,
                                                          .value = "",
                                                          .request_id = request_id}));
      leader_hint = propose.leader_hint;
      if (!propose.Ok()) {
        impl_->inflight_deletes.erase(request_id);
        if (propose.status == kvstore::raft::ProposeStatus::kNotLeader) {
          return MakeNotLeader(propose.leader_hint);
        }
        if (propose.status == kvstore::raft::ProposeStatus::kQuorumUnavailable) {
          return MakeUnavailable("quorum unavailable");
        }
        return Error{.code = ErrorCode::kInternal, .message = "propose failed"};
      }
    }
  }

  const auto status = wait.wait_until(deadline);
  if (status != std::future_status::ready) {
    return Error{.code = ErrorCode::kTimeout,
                 .message = "timed out waiting for commit",
                 .leader_hint = leader_hint};
  }
  return wait.get();
}

auto KvRaftService::Get(const std::string& key) -> Result<GetResult> {
  if (!impl_->initialized) {
    return MakeUnavailable("service not initialized");
  }
  if (key.size() > kMaxKeyBytes) {
    return MakeInvalid("key exceeds 1KiB limit");
  }

  std::lock_guard<std::mutex> lock(impl_->mu);
  auto [leader_id, leader_node] = impl_->LeaderNodeLocked();
  if (leader_node == nullptr) {
    return MakeUnavailable("no leader elected");
  }
  if (leader_node->role() != kvstore::raft::Role::kLeader) {
    return MakeNotLeader(leader_node->leader_id());
  }
  if (!leader_node->HasQuorumContact()) {
    return MakeUnavailable("leader has no quorum contact (linearizable read rejected)");
  }

  const auto it = impl_->nodes.find(leader_id);
  if (it == impl_->nodes.end() || !it->second.engine) {
    return Error{.code = ErrorCode::kInternal, .message = "missing leader engine"};
  }

  const auto value = it->second.engine->Get(key);
  GetResult res;
  res.found = value.has_value();
  if (value.has_value()) {
    res.value = value.value();
  }
  return res;
}

auto KvRaftService::SetNodeUp(kvstore::raft::NodeId id, bool up) -> void {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->cluster) {
    impl_->cluster->SetNodeUp(id, up);
  }
}

auto KvRaftService::FindLeader() const -> std::optional<kvstore::raft::NodeId> {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (!impl_->cluster) {
    return std::nullopt;
  }
  return impl_->cluster->FindLeader();
}

}  // namespace kvstore::service
