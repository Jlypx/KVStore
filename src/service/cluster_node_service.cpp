#include "kvstore/service/cluster_node_service.h"

#include <cstdint>
#include <utility>

#include "kvstore/integrity/crc32c.h"

namespace kvstore::service {
namespace {

constexpr std::size_t kMaxKeyBytes = 1024;
constexpr std::size_t kMaxValueBytes = 1024 * 1024;
// Embedded-mode timings are intentionally aggressive for fast unit tests, but
// real multi-process gRPC traffic needs a less fragile profile to avoid leader
// churn under scheduler jitter and connection setup latency.
constexpr std::uint64_t kClusterElectionTimeoutMinTicks = 15;
constexpr std::uint64_t kClusterElectionTimeoutMaxTicks = 30;
constexpr std::uint64_t kClusterHeartbeatIntervalTicks = 2;
constexpr std::uint64_t kClusterQuorumTimeoutTicks = 15;
constexpr auto kClusterTickPeriod = std::chrono::milliseconds(10);

enum class Op : std::uint8_t {
  kPut = 1,
  kDelete = 2,
};

struct EncodedCommand {
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

auto ReadU32(std::string_view in, std::size_t* offset, std::uint32_t* out) -> bool {
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

auto EncodeCommand(const EncodedCommand& cmd) -> std::string {
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

auto DecodeCommand(std::string_view bytes) -> std::optional<EncodedCommand> {
  if (bytes.size() < 2) {
    return std::nullopt;
  }
  const auto version = static_cast<std::uint8_t>(bytes[0]);
  const auto op = static_cast<std::uint8_t>(bytes[1]);
  if (version != 1) {
    return std::nullopt;
  }
  EncodedCommand cmd;
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

  return off == bytes.size() ? std::optional<EncodedCommand>(cmd) : std::nullopt;
}

auto MakeInvalid(const std::string& message) -> Error {
  return Error{.code = ErrorCode::kInvalidArgument, .message = message};
}

auto MakeUnavailable(const std::string& message) -> Error {
  return Error{.code = ErrorCode::kUnavailable, .message = message};
}

auto MakeNotLeader(kvstore::raft::NodeId hint) -> Error {
  return Error{.code = ErrorCode::kNotLeader,
               .message = "not leader",
               .leader_hint = hint};
}

}  // namespace

ClusterNodeService::ClusterNodeService(kvstore::runtime::ClusterProcessConfig config)
    : config_(std::move(config)) {}

ClusterNodeService::~ClusterNodeService() { Stop(); }

auto ClusterNodeService::Start() -> bool {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (started_) {
    return true;
  }

  // Each process owns exactly one engine and one Raft node; peer replication is
  // delegated to NetworkTransport instead of the in-memory embedded transport.
  const auto engine_dir = config_.data_dir / "engine";
  const auto wal_path = engine_dir / "000001.wal";
  engine_ = std::make_unique<kvstore::engine::KvEngine>(wal_path);
  if (!engine_->Open()) {
    return false;
  }

  std::vector<kvstore::raft::NodeId> node_ids;
  std::unordered_map<kvstore::raft::NodeId, std::string> peer_addrs;
  node_ids.reserve(config_.nodes.size());
  for (const auto& node : config_.nodes) {
    node_ids.push_back(node.node_id);
    if (node.node_id != config_.self_id) {
      peer_addrs[node.node_id] = node.peer_addr;
    }
  }

  transport_ = std::make_unique<kvstore::raft::NetworkTransport>(
      config_.self_id, std::move(peer_addrs),
      [this](kvstore::raft::Message message) {
        std::lock_guard<std::recursive_mutex> response_lock(mu_);
        if (node_) {
          node_->Step(std::move(message));
        }
      });

  kvstore::raft::RaftNodeConfig raft_config;
  raft_config.node_id = config_.self_id;
  raft_config.static_node_ids = std::move(node_ids);
  raft_config.election_timeout_ticks_min = kClusterElectionTimeoutMinTicks;
  raft_config.election_timeout_ticks_max = kClusterElectionTimeoutMaxTicks;
  raft_config.heartbeat_interval_ticks = kClusterHeartbeatIntervalTicks;
  raft_config.quorum_timeout_ticks = kClusterQuorumTimeoutTicks;
  raft_config.random_seed = static_cast<std::uint32_t>(config_.self_id * 101U + 11U);
  raft_config.storage_dir = config_.data_dir / "raft";

  node_ = std::make_unique<kvstore::raft::RaftNode>(
      std::move(raft_config),
      [this](kvstore::raft::Message message) {
        if (transport_) {
          transport_->Send(std::move(message));
        }
      });
  node_->SetOnCommitted([this](std::vector<kvstore::raft::CommittedEntry> entries) {
    this->OnCommitted(std::move(entries));
  });
  node_->SetSnapshotHooks(kvstore::raft::SnapshotHooks{
      .on_snapshot_send_requested =
          [this](kvstore::raft::NodeId target, const kvstore::raft::SnapshotMetadata& base) {
            if (!transport_ || !engine_ || !node_) {
              return;
            }
            std::string payload;
            if (!engine_->ExportSnapshotPayload(&payload)) {
              return;
            }
            // The engine snapshot bytes travel over the same transport as other
            // Raft RPCs; followers install the payload before updating Raft
            // snapshot metadata so storage and log state stay aligned.
            transport_->Send(kvstore::raft::Message{
                .from = config_.self_id,
                .to = target,
                .rpc = kvstore::raft::Rpc{kvstore::raft::InstallSnapshotRequest{
                    .term = node_->current_term(),
                    .leader_id = config_.self_id,
                    .last_included_index = base.last_included_index,
                    .last_included_term = base.last_included_term,
                    .snapshot_payload = std::move(payload),
                }},
            });
          }});
  transport_->RegisterNode(config_.self_id, [this](kvstore::raft::Message message) {
    std::lock_guard<std::recursive_mutex> response_lock(mu_);
    if (node_) {
      node_->Step(std::move(message));
    }
  });

  stop_.store(false);
  ticker_ = std::thread([this] { this->TickLoop(); });
  started_ = true;
  return true;
}

auto ClusterNodeService::Stop() -> void {
  stop_.store(true);
  if (ticker_.joinable()) {
    ticker_.join();
  }
}

auto ClusterNodeService::EnsureStarted() const -> bool { return started_ && node_ && engine_; }

auto ClusterNodeService::TickLoop() -> void {
  while (!stop_.load()) {
    {
      std::lock_guard<std::recursive_mutex> lock(mu_);
      if (node_) {
        node_->Tick();
      }
      if (node_ && node_->role() == kvstore::raft::Role::kLeader &&
          pending_snapshot_.has_value()) {
        // Snapshot creation is deferred out of OnCommitted() so the commit path
        // stays focused on applying entries and waking waiting clients.
        node_->InstallLocalSnapshot(pending_snapshot_.value());
        pending_snapshot_.reset();
      }
    }
    std::this_thread::sleep_for(kClusterTickPeriod);
  }
}

auto ClusterNodeService::OnCommitted(
    std::vector<kvstore::raft::CommittedEntry> entries) -> void {
  if (!engine_) {
    return;
  }

  for (const auto& entry : entries) {
    const auto decoded = DecodeCommand(entry.command);
    if (!decoded.has_value()) {
      continue;
    }
    const auto& cmd = decoded.value();

    if (cmd.op == Op::kPut) {
      const bool existed = engine_->Get(cmd.key).has_value();
      const auto applied = engine_->Put(cmd.key, cmd.value, cmd.request_id);
      if (!applied.Ok()) {
        continue;
      }
      // The API-level completion caches sit outside the engine so retrying the
      // exact same request_id can return the original semantic result.
      PutResult result{.overwritten = existed};
      PutCacheEntry cache_entry;
      cache_entry.result = result;
      cache_entry.key_crc = kvstore::integrity::ComputeCrc32c(cmd.key);
      cache_entry.value_crc = kvstore::integrity::ComputeCrc32c(cmd.value);
      cache_entry.key_size = cmd.key.size();
      cache_entry.value_size = cmd.value.size();
      completed_puts_[cmd.request_id] = cache_entry;

      const auto inflight = inflight_puts_.find(cmd.request_id);
      if (inflight != inflight_puts_.end() && inflight->second.promise) {
        inflight->second.promise->set_value(result);
        inflight_puts_.erase(inflight);
      }
    } else if (cmd.op == Op::kDelete) {
      const bool existed = engine_->Get(cmd.key).has_value();
      const auto applied = engine_->Delete(cmd.key, cmd.request_id);
      if (!applied.Ok()) {
        continue;
      }
      DeleteResult result{.deleted = existed};
      completed_deletes_[cmd.request_id] = result;

      const auto inflight = inflight_deletes_.find(cmd.request_id);
      if (inflight != inflight_deletes_.end() && inflight->second.promise) {
        inflight->second.promise->set_value(result);
        inflight_deletes_.erase(inflight);
      }
    }
  }

  if (node_ && node_->role() == kvstore::raft::Role::kLeader) {
    const auto snapshot = node_->MaybeSnapshotMetadata();
    if (snapshot.has_value()) {
      // Only mark snapshot intent here; the actual truncation runs from TickLoop
      // once the committed entries have been fully applied above.
      pending_snapshot_ = snapshot;
    }
  }
}

auto ClusterNodeService::Put(const std::string& key,
                             const std::string& value,
                             const std::string& request_id,
                             std::chrono::steady_clock::time_point deadline)
    -> Result<PutResult> {
  if (!EnsureStarted()) {
    return MakeUnavailable("cluster node service not started");
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
  const auto key_crc = kvstore::integrity::ComputeCrc32c(key);
  const auto value_crc = kvstore::integrity::ComputeCrc32c(value);

  {
    std::lock_guard<std::recursive_mutex> lock(mu_);

    // completed_* and inflight_* together implement at-most-once semantics for
    // client-visible effects even though clients may retry across timeouts.
    const auto done = completed_puts_.find(request_id);
    if (done != completed_puts_.end()) {
      if (done->second.key_crc != key_crc || done->second.value_crc != value_crc ||
          done->second.key_size != key.size() ||
          done->second.value_size != value.size()) {
        return MakeInvalid("request_id reused with different key/value");
      }
      return done->second.result;
    }

    const auto inflight = inflight_puts_.find(request_id);
    if (inflight != inflight_puts_.end()) {
      if (inflight->second.key_crc != key_crc ||
          inflight->second.value_crc != value_crc ||
          inflight->second.key_size != key.size() ||
          inflight->second.value_size != value.size()) {
        return MakeInvalid("request_id reused with different key/value");
      }
      wait = inflight->second.future;
    } else {
      auto promise = std::make_shared<std::promise<PutResult>>();
      wait = promise->get_future().share();
      inflight_puts_[request_id] = InflightPut{
          .promise = promise,
          .future = wait,
          .key_crc = key_crc,
          .value_crc = value_crc,
          .key_size = key.size(),
          .value_size = value.size(),
      };

      if (node_->role() != kvstore::raft::Role::kLeader) {
        leader_hint = node_->leader_id();
        inflight_puts_.erase(request_id);
        return MakeNotLeader(leader_hint);
      }
      leader_hint = node_->node_id();
      // Publish the waiter before proposing so a very fast local commit cannot
      // race past the caller and leave it waiting on an untracked request.
      const auto propose = node_->Propose(EncodeCommand(EncodedCommand{
          .op = Op::kPut, .key = key, .value = value, .request_id = request_id}));
      leader_hint = propose.leader_hint;
      if (!propose.Ok()) {
        inflight_puts_.erase(request_id);
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

auto ClusterNodeService::Get(const std::string& key) -> Result<GetResult> {
  if (!EnsureStarted()) {
    return MakeUnavailable("cluster node service not started");
  }
  if (key.size() > kMaxKeyBytes) {
    return MakeInvalid("key exceeds 1KiB limit");
  }

  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (node_->role() != kvstore::raft::Role::kLeader) {
    return MakeNotLeader(node_->leader_id());
  }
  // This mirrors KvRaftService::Get(): linearizable reads are served only while
  // the leader still has fresh quorum contact.
  if (!node_->HasQuorumContact()) {
    return MakeUnavailable("leader has no quorum contact (linearizable read rejected)");
  }

  const auto value = engine_->Get(key);
  GetResult result;
  result.found = value.has_value();
  if (value.has_value()) {
    result.value = value.value();
  }
  return result;
}

auto ClusterNodeService::Delete(const std::string& key,
                                const std::string& request_id,
                                std::chrono::steady_clock::time_point deadline)
    -> Result<DeleteResult> {
  if (!EnsureStarted()) {
    return MakeUnavailable("cluster node service not started");
  }
  if (key.size() > kMaxKeyBytes) {
    return MakeInvalid("key exceeds 1KiB limit");
  }
  if (request_id.empty()) {
    return MakeInvalid("request_id must be non-empty");
  }

  std::shared_future<DeleteResult> wait;
  kvstore::raft::NodeId leader_hint = kvstore::raft::kNoLeader;

  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    const auto done = completed_deletes_.find(request_id);
    if (done != completed_deletes_.end()) {
      return done->second;
    }

    const auto inflight = inflight_deletes_.find(request_id);
    if (inflight != inflight_deletes_.end()) {
      wait = inflight->second.future;
    } else {
      auto promise = std::make_shared<std::promise<DeleteResult>>();
      wait = promise->get_future().share();
      inflight_deletes_[request_id] =
          InflightDelete{.promise = promise, .future = wait};

      if (node_->role() != kvstore::raft::Role::kLeader) {
        leader_hint = node_->leader_id();
        inflight_deletes_.erase(request_id);
        return MakeNotLeader(leader_hint);
      }
      leader_hint = node_->node_id();
      const auto propose = node_->Propose(EncodeCommand(EncodedCommand{
          .op = Op::kDelete, .key = key, .value = "", .request_id = request_id}));
      leader_hint = propose.leader_hint;
      if (!propose.Ok()) {
        inflight_deletes_.erase(request_id);
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

auto ClusterNodeService::HandlePeerRequestVote(
    kvstore::raft::NodeId from, const kvstore::raft::RequestVoteRequest& request)
    -> kvstore::raft::RequestVoteResponse {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return node_->HandlePeerRequestVote(from, request);
}

auto ClusterNodeService::HandlePeerAppendEntries(
    kvstore::raft::NodeId from, const kvstore::raft::AppendEntriesRequest& request)
    -> kvstore::raft::AppendEntriesResponse {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return node_->HandlePeerAppendEntries(from, request);
}

auto ClusterNodeService::HandlePeerInstallSnapshot(
    kvstore::raft::NodeId /*from*/,
    const kvstore::raft::InstallSnapshotRequest& request)
    -> kvstore::raft::InstallSnapshotResponse {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  // Install the engine bytes first; only if storage replacement succeeds do we
  // let Raft advance log_base_index_ / log_base_term_ to the snapshot point.
  const bool ok = engine_ && engine_->InstallSnapshotPayload(request.snapshot_payload);
  if (!ok || !node_) {
    return kvstore::raft::InstallSnapshotResponse{
        .term = node_ ? node_->current_term() : request.term,
        .success = false,
        .last_included_index = request.last_included_index,
    };
  }
  return node_->HandlePeerInstallSnapshot(request.leader_id, request);
}

auto ClusterNodeService::FindLeader() const
    -> std::optional<kvstore::raft::NodeId> {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  if (!node_) {
    return std::nullopt;
  }
  if (node_->role() == kvstore::raft::Role::kLeader) {
    return node_->node_id();
  }
  if (node_->leader_id() != kvstore::raft::kNoLeader) {
    return node_->leader_id();
  }
  return std::nullopt;
}

}  // namespace kvstore::service
