#include "kvstore/service/raft_peer_service.h"

#include "kvstore/raft/raft_proto_conversion.h"

namespace kvstore::service {

RaftPeerService::RaftPeerService(RequestVoteHandler request_vote_handler,
                                 AppendEntriesHandler append_entries_handler)
    : request_vote_handler_(std::move(request_vote_handler)),
      append_entries_handler_(std::move(append_entries_handler)) {}

auto RaftPeerService::RequestVote(grpc::ServerContext* /*context*/,
                                  const kvstore::v1::RequestVoteRequest* request,
                                  kvstore::v1::RequestVoteResponse* response)
    -> grpc::Status {
  const auto internal_request = kvstore::raft::FromProto(*request);
  const auto internal_response =
      request_vote_handler_(internal_request.candidate_id, internal_request);
  *response = kvstore::raft::ToProto(internal_response);
  return grpc::Status::OK;
}

auto RaftPeerService::AppendEntries(
    grpc::ServerContext* /*context*/,
    const kvstore::v1::AppendEntriesRequest* request,
    kvstore::v1::AppendEntriesResponse* response) -> grpc::Status {
  const auto internal_request = kvstore::raft::FromProto(*request);
  const auto internal_response =
      append_entries_handler_(internal_request.leader_id, internal_request);
  *response = kvstore::raft::ToProto(internal_response);
  return grpc::Status::OK;
}

}  // namespace kvstore::service
