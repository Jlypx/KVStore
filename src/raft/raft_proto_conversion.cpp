#include "kvstore/raft/raft_proto_conversion.h"

namespace kvstore::raft {

auto ToProto(const LogEntry& entry) -> kvstore::v1::LogEntry {
  kvstore::v1::LogEntry proto;
  proto.set_term(entry.term);
  proto.set_command(entry.command);
  return proto;
}

auto FromProto(const kvstore::v1::LogEntry& entry) -> LogEntry {
  return LogEntry{.term = entry.term(), .command = entry.command()};
}

auto ToProto(const RequestVoteRequest& request)
    -> kvstore::v1::RequestVoteRequest {
  kvstore::v1::RequestVoteRequest proto;
  proto.set_term(request.term);
  proto.set_candidate_id(request.candidate_id);
  proto.set_last_log_index(request.last_log_index);
  proto.set_last_log_term(request.last_log_term);
  return proto;
}

auto FromProto(const kvstore::v1::RequestVoteRequest& request)
    -> RequestVoteRequest {
  return RequestVoteRequest{
      .term = request.term(),
      .candidate_id = request.candidate_id(),
      .last_log_index = request.last_log_index(),
      .last_log_term = request.last_log_term(),
  };
}

auto ToProto(const RequestVoteResponse& response)
    -> kvstore::v1::RequestVoteResponse {
  kvstore::v1::RequestVoteResponse proto;
  proto.set_term(response.term);
  proto.set_vote_granted(response.vote_granted);
  return proto;
}

auto FromProto(const kvstore::v1::RequestVoteResponse& response)
    -> RequestVoteResponse {
  return RequestVoteResponse{
      .term = response.term(),
      .vote_granted = response.vote_granted(),
  };
}

auto ToProto(const AppendEntriesRequest& request)
    -> kvstore::v1::AppendEntriesRequest {
  kvstore::v1::AppendEntriesRequest proto;
  proto.set_term(request.term);
  proto.set_leader_id(request.leader_id);
  proto.set_prev_log_index(request.prev_log_index);
  proto.set_prev_log_term(request.prev_log_term);
  proto.set_leader_commit(request.leader_commit);
  for (const auto& entry : request.entries) {
    *proto.add_entries() = ToProto(entry);
  }
  return proto;
}

auto FromProto(const kvstore::v1::AppendEntriesRequest& request)
    -> AppendEntriesRequest {
  AppendEntriesRequest decoded;
  decoded.term = request.term();
  decoded.leader_id = request.leader_id();
  decoded.prev_log_index = request.prev_log_index();
  decoded.prev_log_term = request.prev_log_term();
  decoded.leader_commit = request.leader_commit();
  decoded.entries.reserve(static_cast<std::size_t>(request.entries_size()));
  for (const auto& entry : request.entries()) {
    decoded.entries.push_back(FromProto(entry));
  }
  return decoded;
}

auto ToProto(const AppendEntriesResponse& response)
    -> kvstore::v1::AppendEntriesResponse {
  kvstore::v1::AppendEntriesResponse proto;
  proto.set_term(response.term);
  proto.set_success(response.success);
  proto.set_match_index(response.match_index);
  return proto;
}

auto FromProto(const kvstore::v1::AppendEntriesResponse& response)
    -> AppendEntriesResponse {
  return AppendEntriesResponse{
      .term = response.term(),
      .success = response.success(),
      .match_index = response.match_index(),
  };
}

auto ToProto(const InstallSnapshotRequest& request)
    -> kvstore::v1::InstallSnapshotRequest {
  kvstore::v1::InstallSnapshotRequest proto;
  proto.set_term(request.term);
  proto.set_leader_id(request.leader_id);
  proto.set_last_included_index(request.last_included_index);
  proto.set_last_included_term(request.last_included_term);
  proto.set_snapshot_payload(request.snapshot_payload);
  return proto;
}

auto FromProto(const kvstore::v1::InstallSnapshotRequest& request)
    -> InstallSnapshotRequest {
  return InstallSnapshotRequest{
      .term = request.term(),
      .leader_id = request.leader_id(),
      .last_included_index = request.last_included_index(),
      .last_included_term = request.last_included_term(),
      .snapshot_payload = request.snapshot_payload(),
  };
}

auto ToProto(const InstallSnapshotResponse& response)
    -> kvstore::v1::InstallSnapshotResponse {
  kvstore::v1::InstallSnapshotResponse proto;
  proto.set_term(response.term);
  proto.set_success(response.success);
  proto.set_last_included_index(response.last_included_index);
  return proto;
}

auto FromProto(const kvstore::v1::InstallSnapshotResponse& response)
    -> InstallSnapshotResponse {
  return InstallSnapshotResponse{
      .term = response.term(),
      .success = response.success(),
      .last_included_index = response.last_included_index(),
  };
}

}  // namespace kvstore::raft
