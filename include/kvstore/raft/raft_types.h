#ifndef KVSTORE_RAFT_RAFT_TYPES_H
#define KVSTORE_RAFT_RAFT_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace kvstore::raft {

using NodeId = std::uint32_t;
using Term = std::uint64_t;
using LogIndex = std::uint64_t;

constexpr NodeId kNoLeader = 0;
constexpr NodeId kNoVote = 0;

enum class Role {
  kFollower,
  kCandidate,
  kLeader,
};

enum class ProposeStatus {
  kOk,
  kNotLeader,
  kQuorumUnavailable,
};

struct ProposeResult {
  ProposeStatus status = ProposeStatus::kOk;
  Term term = 0;
  LogIndex index = 0;
  NodeId leader_hint = kNoLeader;

  [[nodiscard]] auto Ok() const -> bool {
    return status == ProposeStatus::kOk;
  }
};

struct LogEntry {
  Term term = 0;
  std::string command;
};

struct CommittedEntry {
  LogIndex index = 0;
  Term term = 0;
  std::string command;
};

struct SnapshotMetadata {
  LogIndex last_included_index = 0;
  Term last_included_term = 0;
};

struct RequestVoteRequest {
  Term term = 0;
  NodeId candidate_id = 0;
  LogIndex last_log_index = 0;
  Term last_log_term = 0;
};

struct RequestVoteResponse {
  Term term = 0;
  bool vote_granted = false;
};

struct AppendEntriesRequest {
  Term term = 0;
  NodeId leader_id = kNoLeader;
  LogIndex prev_log_index = 0;
  Term prev_log_term = 0;
  std::vector<LogEntry> entries;
  LogIndex leader_commit = 0;
};

struct AppendEntriesResponse {
  Term term = 0;
  bool success = false;
  LogIndex match_index = 0;
};

struct InstallSnapshotRequest {
  Term term = 0;
  NodeId leader_id = kNoLeader;
  LogIndex last_included_index = 0;
  Term last_included_term = 0;
  std::string snapshot_payload;
};

struct InstallSnapshotResponse {
  Term term = 0;
  bool success = false;
  LogIndex last_included_index = 0;
};

using Rpc = std::variant<RequestVoteRequest, RequestVoteResponse,
                         AppendEntriesRequest, AppendEntriesResponse,
                         InstallSnapshotRequest, InstallSnapshotResponse>;

struct Message {
  NodeId from = 0;
  NodeId to = 0;
  Rpc rpc;
};

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_RAFT_TYPES_H
