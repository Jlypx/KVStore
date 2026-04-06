#ifndef KVSTORE_SERVICE_RAFT_PEER_SERVICE_H
#define KVSTORE_SERVICE_RAFT_PEER_SERVICE_H

#include <functional>

#include <grpcpp/grpcpp.h>

#include "kvstore/raft/raft_types.h"
#include "kvstore/v1/raft.grpc.pb.h"

namespace kvstore::service {

class RaftPeerService final : public kvstore::v1::RaftPeer::Service {
 public:
  using RequestVoteHandler = std::function<kvstore::raft::RequestVoteResponse(
      kvstore::raft::NodeId from, const kvstore::raft::RequestVoteRequest& request)>;
  using AppendEntriesHandler = std::function<kvstore::raft::AppendEntriesResponse(
      kvstore::raft::NodeId from, const kvstore::raft::AppendEntriesRequest& request)>;
  using InstallSnapshotHandler = std::function<kvstore::raft::InstallSnapshotResponse(
      kvstore::raft::NodeId from, const kvstore::raft::InstallSnapshotRequest& request)>;

  RaftPeerService(RequestVoteHandler request_vote_handler,
                  AppendEntriesHandler append_entries_handler,
                  InstallSnapshotHandler install_snapshot_handler);

  auto RequestVote(grpc::ServerContext* context,
                   const kvstore::v1::RequestVoteRequest* request,
                   kvstore::v1::RequestVoteResponse* response) -> grpc::Status override;

  auto AppendEntries(grpc::ServerContext* context,
                     const kvstore::v1::AppendEntriesRequest* request,
                     kvstore::v1::AppendEntriesResponse* response) -> grpc::Status override;

  auto InstallSnapshot(
      grpc::ServerContext* context,
      const kvstore::v1::InstallSnapshotRequest* request,
      kvstore::v1::InstallSnapshotResponse* response) -> grpc::Status override;

 private:
  RequestVoteHandler request_vote_handler_;
  AppendEntriesHandler append_entries_handler_;
  InstallSnapshotHandler install_snapshot_handler_;
};

}  // namespace kvstore::service

#endif  // KVSTORE_SERVICE_RAFT_PEER_SERVICE_H
