#ifndef KVSTORE_RAFT_RAFT_NODE_H
#define KVSTORE_RAFT_RAFT_NODE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kvstore/raft/raft_types.h"

namespace kvstore::raft {

struct RaftNodeConfig {
  NodeId node_id = 0;
  std::vector<NodeId> static_node_ids;

  std::uint64_t election_timeout_ticks_min = 10;
  std::uint64_t election_timeout_ticks_max = 20;
  std::uint64_t heartbeat_interval_ticks = 2;

  // If 0, defaults to election_timeout_ticks_min.
  std::uint64_t quorum_timeout_ticks = 0;

  std::uint32_t random_seed = 0;
};

struct SnapshotHooks {
  // Hook point for a future snapshot system.
  //
  // TODO(Task 6+): implement InstallSnapshot RPC and log compaction.
  std::function<void(NodeId target, const SnapshotMetadata& base)>
      on_snapshot_send_requested;
};

class RaftNode {
 public:
  using SendFn = std::function<void(Message)>;
  using CommitCallback = std::function<void(std::vector<CommittedEntry>)>;

  explicit RaftNode(RaftNodeConfig config, SendFn send);

  auto Tick() -> void;
  auto Step(const Message& message) -> void;

  auto Propose(std::string command) -> ProposeResult;

  auto SetOnCommitted(CommitCallback callback) -> void {
    on_committed_ = std::move(callback);
  }

  auto SetSnapshotHooks(SnapshotHooks hooks) -> void {
    snapshot_hooks_ = std::move(hooks);
  }

  [[nodiscard]] auto node_id() const -> NodeId { return config_.node_id; }
  [[nodiscard]] auto role() const -> Role { return role_; }
  [[nodiscard]] auto current_term() const -> Term { return current_term_; }
  [[nodiscard]] auto leader_id() const -> NodeId { return leader_id_; }
  [[nodiscard]] auto committed_index() const -> LogIndex { return commit_index_; }
  [[nodiscard]] auto last_applied_index() const -> LogIndex {
    return last_applied_;
  }

  [[nodiscard]] auto last_log_index() const -> LogIndex {
    return static_cast<LogIndex>(log_.size() - 1U);
  }

  [[nodiscard]] auto last_log_term() const -> Term { return log_.back().term; }

  [[nodiscard]] auto HasQuorumContact() const -> bool;

  [[nodiscard]] auto log_entry_at(LogIndex index) const
      -> std::optional<LogEntry>;

 private:
  [[nodiscard]] auto ClusterSize() const -> std::size_t {
    return config_.static_node_ids.size();
  }

  [[nodiscard]] auto MajorityCount() const -> std::size_t {
    return ClusterSize() / 2U + 1U;
  }

  auto ResetElectionTimeout() -> void;

  auto BecomeFollower(Term term, NodeId leader) -> void;
  auto StartElection() -> void;
  auto BecomeLeader() -> void;

  auto SendTo(NodeId to, Rpc rpc) -> void;

  auto HandleRequestVote(NodeId from, const RequestVoteRequest& request) -> void;
  auto HandleRequestVoteResponse(NodeId from,
                                 const RequestVoteResponse& response) -> void;
  auto HandleAppendEntries(NodeId from,
                           const AppendEntriesRequest& request) -> void;
  auto HandleAppendEntriesResponse(NodeId from,
                                   const AppendEntriesResponse& response) -> void;

  [[nodiscard]] auto IsLogUpToDate(LogIndex last_index, Term last_term) const
      -> bool;

  auto LeaderSendAppendEntries(NodeId follower_id) -> void;
  auto LeaderBroadcastHeartbeats() -> void;
  auto AdvanceCommitIndex() -> void;
  auto ApplyCommitted() -> void;

  RaftNodeConfig config_;
  std::vector<NodeId> peers_;
  SendFn send_;

  CommitCallback on_committed_;
  SnapshotHooks snapshot_hooks_;

  Role role_ = Role::kFollower;
  Term current_term_ = 0;
  NodeId voted_for_ = kNoVote;
  NodeId leader_id_ = kNoLeader;

  // Raft log. Index 0 is a dummy entry with term=0.
  std::vector<LogEntry> log_;

  LogIndex commit_index_ = 0;
  LogIndex last_applied_ = 0;

  std::uint64_t tick_ = 0;
  std::uint64_t election_elapsed_ = 0;
  std::uint64_t election_timeout_ = 0;
  std::uint64_t heartbeat_elapsed_ = 0;

  std::size_t votes_granted_ = 0;

  std::unordered_map<NodeId, LogIndex> next_index_;
  std::unordered_map<NodeId, LogIndex> match_index_;
  std::unordered_map<NodeId, std::uint64_t> last_contact_tick_;

  std::mt19937 rng_;
};

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_RAFT_NODE_H
