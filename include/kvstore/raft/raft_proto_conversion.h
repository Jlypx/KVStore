#ifndef KVSTORE_RAFT_RAFT_PROTO_CONVERSION_H
#define KVSTORE_RAFT_RAFT_PROTO_CONVERSION_H

#include "kvstore/raft/raft_types.h"
#include "kvstore/v1/raft.pb.h"

namespace kvstore::raft {

auto ToProto(const LogEntry& entry) -> kvstore::v1::LogEntry;
auto FromProto(const kvstore::v1::LogEntry& entry) -> LogEntry;

auto ToProto(const RequestVoteRequest& request)
    -> kvstore::v1::RequestVoteRequest;
auto FromProto(const kvstore::v1::RequestVoteRequest& request)
    -> RequestVoteRequest;

auto ToProto(const RequestVoteResponse& response)
    -> kvstore::v1::RequestVoteResponse;
auto FromProto(const kvstore::v1::RequestVoteResponse& response)
    -> RequestVoteResponse;

auto ToProto(const AppendEntriesRequest& request)
    -> kvstore::v1::AppendEntriesRequest;
auto FromProto(const kvstore::v1::AppendEntriesRequest& request)
    -> AppendEntriesRequest;

auto ToProto(const AppendEntriesResponse& response)
    -> kvstore::v1::AppendEntriesResponse;
auto FromProto(const kvstore::v1::AppendEntriesResponse& response)
    -> AppendEntriesResponse;

auto ToProto(const InstallSnapshotRequest& request)
    -> kvstore::v1::InstallSnapshotRequest;
auto FromProto(const kvstore::v1::InstallSnapshotRequest& request)
    -> InstallSnapshotRequest;

auto ToProto(const InstallSnapshotResponse& response)
    -> kvstore::v1::InstallSnapshotResponse;
auto FromProto(const kvstore::v1::InstallSnapshotResponse& response)
    -> InstallSnapshotResponse;

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_RAFT_PROTO_CONVERSION_H
