#include <iostream>
#include <string>

#include "kvstore/raft/raft_proto_conversion.h"
#include "kvstore/raft/raft_types.h"
#include "kvstore/v1/raft.pb.h"

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto TestRequestVoteRoundTrip() -> bool {
  kvstore::raft::RequestVoteRequest request;
  request.term = 7;
  request.candidate_id = 3;
  request.last_log_index = 42;
  request.last_log_term = 6;

  const auto proto = kvstore::raft::ToProto(request);
  const auto decoded = kvstore::raft::FromProto(proto);

  return Expect(proto.term() == 7, "proto term should match") &&
         Expect(proto.candidate_id() == 3, "proto candidate should match") &&
         Expect(decoded.term == request.term, "decoded term should match") &&
         Expect(decoded.candidate_id == request.candidate_id,
                "decoded candidate should match") &&
         Expect(decoded.last_log_index == request.last_log_index,
                "decoded last_log_index should match") &&
         Expect(decoded.last_log_term == request.last_log_term,
                "decoded last_log_term should match");
}

auto TestAppendEntriesRoundTrip() -> bool {
  kvstore::raft::AppendEntriesRequest request;
  request.term = 9;
  request.leader_id = 4;
  request.prev_log_index = 100;
  request.prev_log_term = 8;
  request.leader_commit = 95;
  request.entries.push_back(kvstore::raft::LogEntry{.term = 8, .command = "cmd-a"});
  request.entries.push_back(kvstore::raft::LogEntry{.term = 9, .command = "cmd-b"});

  const auto proto = kvstore::raft::ToProto(request);
  const auto decoded = kvstore::raft::FromProto(proto);

  return Expect(proto.entries_size() == 2, "proto entries should round-trip") &&
         Expect(decoded.term == request.term, "decoded term should match") &&
         Expect(decoded.leader_id == request.leader_id, "decoded leader should match") &&
         Expect(decoded.prev_log_index == request.prev_log_index,
                "decoded prev_log_index should match") &&
         Expect(decoded.prev_log_term == request.prev_log_term,
                "decoded prev_log_term should match") &&
         Expect(decoded.leader_commit == request.leader_commit,
                "decoded leader_commit should match") &&
         Expect(decoded.entries.size() == 2, "decoded entry count should match") &&
         Expect(decoded.entries[0].command == "cmd-a",
                "first decoded command should match") &&
         Expect(decoded.entries[1].command == "cmd-b",
                "second decoded command should match");
}

auto TestAppendEntriesResponseRoundTrip() -> bool {
  kvstore::raft::AppendEntriesResponse response;
  response.term = 11;
  response.success = true;
  response.match_index = 123;

  const auto proto = kvstore::raft::ToProto(response);
  const auto decoded = kvstore::raft::FromProto(proto);

  return Expect(proto.term() == 11, "response proto term should match") &&
         Expect(decoded.term == response.term, "response decoded term should match") &&
         Expect(decoded.success == response.success,
                "response decoded success should match") &&
         Expect(decoded.match_index == response.match_index,
                "response decoded match_index should match");
}

auto TestInstallSnapshotRoundTrip() -> bool {
  kvstore::raft::InstallSnapshotRequest request;
  request.term = 12;
  request.leader_id = 4;
  request.last_included_index = 128;
  request.last_included_term = 11;
  request.snapshot_payload = "snapshot-bytes";

  const auto proto = kvstore::raft::ToProto(request);
  const auto decoded = kvstore::raft::FromProto(proto);

  kvstore::raft::InstallSnapshotResponse response;
  response.term = 12;
  response.success = true;
  response.last_included_index = 128;

  const auto response_proto = kvstore::raft::ToProto(response);
  const auto response_decoded = kvstore::raft::FromProto(response_proto);

  return Expect(proto.term() == request.term, "snapshot proto term should match") &&
         Expect(decoded.last_included_index == request.last_included_index,
                "snapshot decoded last_included_index should match") &&
         Expect(decoded.last_included_term == request.last_included_term,
                "snapshot decoded last_included_term should match") &&
         Expect(decoded.snapshot_payload == request.snapshot_payload,
                "snapshot decoded payload should match") &&
         Expect(response_decoded.success, "snapshot response success should match") &&
         Expect(response_decoded.last_included_index == response.last_included_index,
                "snapshot response last_included_index should match");
}

}  // namespace

int main() {
  if (!TestRequestVoteRoundTrip()) {
    return 1;
  }
  if (!TestAppendEntriesRoundTrip()) {
    return 1;
  }
  if (!TestAppendEntriesResponseRoundTrip()) {
    return 1;
  }
  if (!TestInstallSnapshotRoundTrip()) {
    return 1;
  }
  return 0;
}
