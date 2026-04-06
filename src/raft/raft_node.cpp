#include "kvstore/raft/raft_node.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace kvstore::raft {
namespace {

auto ContainsNodeId(const std::vector<NodeId>& ids, NodeId target) -> bool {
  for (NodeId id : ids) {
    if (id == target) {
      return true;
    }
  }
  return false;
}

}  // namespace

RaftNode::RaftNode(RaftNodeConfig config, SendFn send)
    : config_(std::move(config)), send_(std::move(send)), rng_(config_.random_seed) {
  if (config_.quorum_timeout_ticks == 0) {
    config_.quorum_timeout_ticks = config_.election_timeout_ticks_min;
  }

  if (!ContainsNodeId(config_.static_node_ids, config_.node_id)) {
    // Misconfiguration. Keep node inert rather than crashing.
    config_.static_node_ids.push_back(config_.node_id);
  }

  peers_.reserve(config_.static_node_ids.size());
  for (NodeId id : config_.static_node_ids) {
    if (id != config_.node_id) {
      peers_.push_back(id);
    }
  }

  if (!config_.storage_dir.empty()) {
    storage_ = std::make_unique<RaftStorage>(config_.storage_dir);
    PersistentRaftState persisted;
    if (storage_->Load(&persisted)) {
      current_term_ = persisted.current_term;
      voted_for_ = persisted.voted_for;
      log_base_index_ = persisted.snapshot_last_included_index;
      log_base_term_ = persisted.snapshot_last_included_term;
      log_ = std::move(persisted.log);
    }
  }

  if (log_.empty()) {
    log_.push_back(LogEntry{.term = log_base_term_, .command = ""});
  }
  ResetElectionTimeout();
}

auto RaftNode::ResetElectionTimeout() -> void {
  const auto min_ticks = config_.election_timeout_ticks_min;
  const auto max_ticks = config_.election_timeout_ticks_max;

  const auto safe_min = std::max<std::uint64_t>(1U, min_ticks);
  const auto safe_max = std::max<std::uint64_t>(safe_min, max_ticks);
  std::uniform_int_distribution<std::uint64_t> dist(safe_min, safe_max);
  election_timeout_ = dist(rng_);
  election_elapsed_ = 0;
}

auto RaftNode::Tick() -> void {
  tick_ += 1;

  if (role_ == Role::kLeader) {
    heartbeat_elapsed_ += 1;
    if (heartbeat_elapsed_ >= config_.heartbeat_interval_ticks) {
      heartbeat_elapsed_ = 0;
      LeaderBroadcastHeartbeats();
    }
    return;
  }

  election_elapsed_ += 1;
  if (election_elapsed_ >= election_timeout_) {
    StartElection();
  }
}

auto RaftNode::Step(const Message& message) -> void {
  if (message.to != config_.node_id) {
    return;
  }

  std::visit(
      [this, &message](const auto& rpc) {
        using T = std::decay_t<decltype(rpc)>;
        if constexpr (std::is_same_v<T, RequestVoteRequest>) {
          HandleRequestVote(message.from, rpc);
        } else if constexpr (std::is_same_v<T, RequestVoteResponse>) {
          HandleRequestVoteResponse(message.from, rpc);
        } else if constexpr (std::is_same_v<T, AppendEntriesRequest>) {
          HandleAppendEntries(message.from, rpc);
        } else if constexpr (std::is_same_v<T, AppendEntriesResponse>) {
          HandleAppendEntriesResponse(message.from, rpc);
        } else if constexpr (std::is_same_v<T, InstallSnapshotRequest>) {
          SendTo(message.from, Rpc{BuildInstallSnapshotResponse(message.from, rpc)});
        } else if constexpr (std::is_same_v<T, InstallSnapshotResponse>) {
          HandleAppendEntriesResponse(
              message.from,
              AppendEntriesResponse{.term = rpc.term,
                                    .success = rpc.success,
                                    .match_index = rpc.last_included_index});
        }
      },
      message.rpc);
}

auto RaftNode::SendTo(NodeId to, Rpc rpc) -> void {
  if (!send_) {
    return;
  }
  send_(Message{.from = config_.node_id, .to = to, .rpc = std::move(rpc)});
}

auto RaftNode::BecomeFollower(Term term, NodeId leader) -> void {
  role_ = Role::kFollower;
  if (term > current_term_) {
    current_term_ = term;
    voted_for_ = kNoVote;
  } else {
    current_term_ = term;
  }
  leader_id_ = leader;
  votes_granted_ = 0;
  heartbeat_elapsed_ = 0;
  PersistMetadata();
  ResetElectionTimeout();
}

auto RaftNode::StartElection() -> void {
  role_ = Role::kCandidate;
  leader_id_ = kNoLeader;
  current_term_ += 1;
  voted_for_ = config_.node_id;
  votes_granted_ = 1;
  PersistMetadata();
  ResetElectionTimeout();

  const auto last_index = last_log_index();
  const auto last_term = last_log_term();

  for (NodeId peer : peers_) {
    RequestVoteRequest request;
    request.term = current_term_;
    request.candidate_id = config_.node_id;
    request.last_log_index = last_index;
    request.last_log_term = last_term;
    SendTo(peer, Rpc{std::move(request)});
  }
}

auto RaftNode::BecomeLeader() -> void {
  role_ = Role::kLeader;
  leader_id_ = config_.node_id;
  votes_granted_ = 0;

  log_.push_back(LogEntry{.term = current_term_, .command = ""});
  PersistLog();

  next_index_.clear();
  match_index_.clear();
  last_contact_tick_.clear();

  const auto next = last_log_index() + 1;
  for (NodeId peer : peers_) {
    next_index_[peer] = next;
    match_index_[peer] = 0;
    last_contact_tick_[peer] = 0;
  }

  heartbeat_elapsed_ = 0;
  LeaderBroadcastHeartbeats();
}

auto RaftNode::IsLogUpToDate(LogIndex last_index, Term last_term) const -> bool {
  const auto my_last_term = last_log_term();
  if (last_term != my_last_term) {
    return last_term > my_last_term;
  }
  return last_index >= last_log_index();
}

auto RaftNode::LogOffset(LogIndex index) const -> std::optional<std::size_t> {
  if (index < log_base_index_ || index > last_log_index()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(index - log_base_index_);
}

auto RaftNode::TermAt(LogIndex index) const -> std::optional<Term> {
  const auto offset = LogOffset(index);
  if (!offset.has_value()) {
    return std::nullopt;
  }
  return log_[offset.value()].term;
}

auto RaftNode::HandleRequestVote(NodeId from,
                                 const RequestVoteRequest& request) -> void {
  SendTo(from, Rpc{BuildRequestVoteResponse(from, request)});
}

auto RaftNode::HandleRequestVoteResponse(
    NodeId /*from*/, const RequestVoteResponse& response) -> void {
  if (role_ != Role::kCandidate) {
    return;
  }

  if (response.term > current_term_) {
    BecomeFollower(response.term, kNoLeader);
    return;
  }

  if (response.term < current_term_) {
    return;
  }

  if (!response.vote_granted) {
    return;
  }

  votes_granted_ += 1;
  if (votes_granted_ >= MajorityCount()) {
    BecomeLeader();
  }
}

auto RaftNode::HandleAppendEntries(NodeId from,
                                   const AppendEntriesRequest& request) -> void {
  SendTo(from, Rpc{BuildAppendEntriesResponse(from, request)});
}

auto RaftNode::LeaderSendAppendEntries(NodeId follower_id) -> void {
  const auto it = next_index_.find(follower_id);
  if (it == next_index_.end()) {
    return;
  }

  LogIndex next = it->second;
  if (next <= log_base_index_) {
    if (snapshot_hooks_.on_snapshot_send_requested) {
      snapshot_hooks_.on_snapshot_send_requested(
          follower_id, SnapshotMetadata{.last_included_index = log_base_index_,
                                        .last_included_term = log_base_term_});
    }
    return;
  }

  const auto last = last_log_index();
  if (next > last + 1) {
    next = last + 1;
    it->second = next;
  }

  const LogIndex prev = next - 1;

  AppendEntriesRequest request;
  request.term = current_term_;
  request.leader_id = config_.node_id;
  request.prev_log_index = prev;
  request.prev_log_term = TermAt(request.prev_log_index).value_or(log_base_term_);
  request.leader_commit = commit_index_;

  for (LogIndex idx = next; idx <= last; ++idx) {
    const auto offset = LogOffset(idx);
    if (offset.has_value()) {
      request.entries.push_back(log_[offset.value()]);
    }
  }

  SendTo(follower_id, Rpc{std::move(request)});
}

auto RaftNode::LeaderBroadcastHeartbeats() -> void {
  if (role_ != Role::kLeader) {
    return;
  }
  for (NodeId peer : peers_) {
    LeaderSendAppendEntries(peer);
  }
}

auto RaftNode::HandleAppendEntriesResponse(
    NodeId from, const AppendEntriesResponse& response) -> void {
  if (role_ != Role::kLeader) {
    return;
  }

  if (response.term > current_term_) {
    BecomeFollower(response.term, kNoLeader);
    return;
  }

  if (response.term < current_term_) {
    return;
  }

  if (!response.success) {
    auto it = next_index_.find(from);
    if (it != next_index_.end()) {
      const LogIndex current_next = it->second;
      it->second = current_next > 1 ? current_next - 1 : 1;
      LeaderSendAppendEntries(from);
    }
    return;
  }

  match_index_[from] = response.match_index;
  next_index_[from] = response.match_index + 1;
  last_contact_tick_[from] = tick_;

  AdvanceCommitIndex();
}

auto RaftNode::AdvanceCommitIndex() -> void {
  if (role_ != Role::kLeader) {
    return;
  }

  for (LogIndex idx = last_log_index(); idx > commit_index_; --idx) {
    const auto term = TermAt(idx);
    if (!term.has_value() || term.value() != current_term_) {
      continue;
    }

    std::size_t replicated = 1;  // leader itself
    for (NodeId peer : peers_) {
      const auto it = match_index_.find(peer);
      if (it != match_index_.end() && it->second >= idx) {
        replicated += 1;
      }
    }

    if (replicated >= MajorityCount()) {
      commit_index_ = idx;
      ApplyCommitted();
      break;
    }
  }
}

auto RaftNode::ApplyCommitted() -> void {
  if (commit_index_ <= last_applied_) {
    return;
  }

  std::vector<CommittedEntry> committed;
  for (LogIndex idx = last_applied_ + 1; idx <= commit_index_; ++idx) {
    const auto offset = LogOffset(idx);
    if (!offset.has_value()) {
      continue;
    }
    const auto& entry = log_[offset.value()];
    committed.push_back(CommittedEntry{.index = idx,
                                       .term = entry.term,
                                       .command = entry.command});
  }

  last_applied_ = commit_index_;
  if (on_committed_) {
    on_committed_(std::move(committed));
  }
}

auto RaftNode::HasQuorumContact() const -> bool {
  if (role_ != Role::kLeader) {
    return false;
  }

  const auto window = config_.quorum_timeout_ticks;
  std::size_t contact = 1;  // self
  for (NodeId peer : peers_) {
    const auto it = last_contact_tick_.find(peer);
    if (it == last_contact_tick_.end()) {
      continue;
    }
    const auto last = it->second;
    if (last == 0) {
      continue;
    }
    if (tick_ >= last && (tick_ - last) <= window) {
      contact += 1;
    }
  }

  return contact >= MajorityCount();
}

auto RaftNode::log_entry_at(LogIndex index) const -> std::optional<LogEntry> {
  const auto offset = LogOffset(index);
  if (!offset.has_value()) {
    return std::nullopt;
  }
  return log_[offset.value()];
}

auto RaftNode::MaybeSnapshotMetadata() const -> std::optional<SnapshotMetadata> {
  if (commit_index_ <= log_base_index_) {
    return std::nullopt;
  }
  if ((commit_index_ - log_base_index_) < config_.snapshot_threshold_entries) {
    return std::nullopt;
  }
  const auto term = TermAt(commit_index_);
  if (!term.has_value()) {
    return std::nullopt;
  }
  return SnapshotMetadata{
      .last_included_index = commit_index_,
      .last_included_term = term.value(),
  };
}

auto RaftNode::InstallLocalSnapshot(const SnapshotMetadata& metadata) -> void {
  if (metadata.last_included_index <= log_base_index_) {
    return;
  }

  std::vector<LogEntry> new_log;
  const auto suffix_start = metadata.last_included_index + 1;
  new_log.push_back(LogEntry{.term = metadata.last_included_term, .command = ""});
  for (LogIndex index = suffix_start; index <= last_log_index(); ++index) {
    const auto offset = LogOffset(index);
    if (offset.has_value()) {
      new_log.push_back(log_[offset.value()]);
    }
  }

  log_base_index_ = metadata.last_included_index;
  log_base_term_ = metadata.last_included_term;
  log_ = std::move(new_log);
  commit_index_ = std::max(commit_index_, log_base_index_);
  last_applied_ = std::max(last_applied_, log_base_index_);
  PersistMetadata();
  PersistLog();
}

auto RaftNode::Propose(std::string command) -> ProposeResult {
  ProposeResult result;
  result.term = current_term_;

  if (role_ != Role::kLeader) {
    result.status = ProposeStatus::kNotLeader;
    result.leader_hint = leader_id_;
    return result;
  }

  result.leader_hint = config_.node_id;
  if (!HasQuorumContact()) {
    result.status = ProposeStatus::kQuorumUnavailable;
    return result;
  }

  log_.push_back(LogEntry{.term = current_term_, .command = std::move(command)});
  PersistLog();
  result.index = last_log_index();
  result.status = ProposeStatus::kOk;

  for (NodeId peer : peers_) {
    LeaderSendAppendEntries(peer);
  }

  return result;
}

auto RaftNode::HandlePeerRequestVote(NodeId from,
                                     const RequestVoteRequest& request)
    -> RequestVoteResponse {
  return BuildRequestVoteResponse(from, request);
}

auto RaftNode::HandlePeerAppendEntries(NodeId from,
                                       const AppendEntriesRequest& request)
    -> AppendEntriesResponse {
  return BuildAppendEntriesResponse(from, request);
}

auto RaftNode::HandlePeerInstallSnapshot(
    NodeId from, const InstallSnapshotRequest& request)
    -> InstallSnapshotResponse {
  return BuildInstallSnapshotResponse(from, request);
}

auto RaftNode::PersistMetadata() -> void {
  if (storage_) {
    storage_->StoreMetadata(current_term_, voted_for_, log_base_index_, log_base_term_);
  }
}

auto RaftNode::PersistLog() -> void {
  if (storage_) {
    storage_->StoreLog(log_);
  }
}

auto RaftNode::BuildRequestVoteResponse(
    NodeId /*from*/, const RequestVoteRequest& request) -> RequestVoteResponse {
  if (request.term < current_term_) {
    return RequestVoteResponse{.term = current_term_, .vote_granted = false};
  }

  if (request.term > current_term_) {
    BecomeFollower(request.term, kNoLeader);
  }

  const bool can_vote = (voted_for_ == kNoVote || voted_for_ == request.candidate_id);
  const bool up_to_date = IsLogUpToDate(request.last_log_index, request.last_log_term);
  const bool grant = can_vote && up_to_date;

  if (grant) {
    voted_for_ = request.candidate_id;
    PersistMetadata();
    election_elapsed_ = 0;
  }

  return RequestVoteResponse{.term = current_term_, .vote_granted = grant};
}

auto RaftNode::BuildAppendEntriesResponse(
    NodeId /*from*/, const AppendEntriesRequest& request) -> AppendEntriesResponse {
  if (request.term < current_term_) {
    return AppendEntriesResponse{
        .term = current_term_, .success = false, .match_index = 0};
  }

  if (request.term > current_term_ || role_ != Role::kFollower) {
    BecomeFollower(request.term, request.leader_id);
  }

  leader_id_ = request.leader_id;
  election_elapsed_ = 0;

  if (request.prev_log_index > last_log_index()) {
    return AppendEntriesResponse{
        .term = current_term_, .success = false, .match_index = last_log_index()};
  }

  const auto prev_term = TermAt(request.prev_log_index);
  if (!prev_term.has_value() || prev_term.value() != request.prev_log_term) {
    return AppendEntriesResponse{
        .term = current_term_, .success = false, .match_index = log_base_index_};
  }

  LogIndex index = request.prev_log_index + 1;
  std::size_t entry_offset = 0;
  bool log_changed = false;
  while (entry_offset < request.entries.size() && index <= last_log_index()) {
    const auto& entry = request.entries[entry_offset];
    const auto offset = LogOffset(index);
    if (!offset.has_value() || log_[offset.value()].term != entry.term) {
      if (offset.has_value()) {
        const auto keep = std::max<std::size_t>(1U, offset.value());
        log_.resize(keep);
      }
      log_changed = true;
      break;
    }
    index += 1;
    entry_offset += 1;
  }

  while (entry_offset < request.entries.size()) {
    log_.push_back(request.entries[entry_offset]);
    entry_offset += 1;
    log_changed = true;
  }

  if (log_changed) {
    PersistLog();
  }

  if (request.leader_commit > commit_index_) {
    commit_index_ = std::min(request.leader_commit, last_log_index());
    ApplyCommitted();
  }

  const LogIndex appended_last =
      request.prev_log_index + static_cast<LogIndex>(request.entries.size());
  return AppendEntriesResponse{
      .term = current_term_, .success = true, .match_index = appended_last};
}

auto RaftNode::BuildInstallSnapshotResponse(
    NodeId /*from*/, const InstallSnapshotRequest& request)
    -> InstallSnapshotResponse {
  if (request.term < current_term_) {
    return InstallSnapshotResponse{
        .term = current_term_,
        .success = false,
        .last_included_index = log_base_index_,
    };
  }

  if (request.term > current_term_ || role_ != Role::kFollower) {
    BecomeFollower(request.term, request.leader_id);
  }

  leader_id_ = request.leader_id;
  election_elapsed_ = 0;
  InstallLocalSnapshot(SnapshotMetadata{
      .last_included_index = request.last_included_index,
      .last_included_term = request.last_included_term,
  });

  return InstallSnapshotResponse{
      .term = current_term_,
      .success = true,
      .last_included_index = log_base_index_,
  };
}

}  // namespace kvstore::raft
