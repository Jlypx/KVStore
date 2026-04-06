#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "kvstore/raft/network_transport.h"
#include "kvstore/raft/raft_node.h"
#include "kvstore/service/raft_peer_service.h"

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

struct PeerServerHandle {
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<kvstore::service::RaftPeerService> service;
  int port = 0;
};

auto StartPeerServer(kvstore::raft::RaftNode* node) -> PeerServerHandle {
  PeerServerHandle handle;
  handle.service = std::make_unique<kvstore::service::RaftPeerService>(
      [node](kvstore::raft::NodeId from,
             const kvstore::raft::RequestVoteRequest& request) {
        return node->HandlePeerRequestVote(from, request);
      },
      [node](kvstore::raft::NodeId from,
             const kvstore::raft::AppendEntriesRequest& request) {
        return node->HandlePeerAppendEntries(from, request);
      });

  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &handle.port);
  builder.RegisterService(handle.service.get());
  handle.server = builder.BuildAndStart();
  return handle;
}

auto TestNetworkTransportRoundTrip() -> bool {
  kvstore::raft::RaftNodeConfig follower_config;
  follower_config.node_id = 2;
  follower_config.static_node_ids = {1, 2, 3, 4, 5};
  follower_config.election_timeout_ticks_min = 5;
  follower_config.election_timeout_ticks_max = 10;
  follower_config.heartbeat_interval_ticks = 1;
  follower_config.quorum_timeout_ticks = 5;
  follower_config.random_seed = 123;

  kvstore::raft::RaftNode follower(follower_config, [](kvstore::raft::Message) {});
  auto server = StartPeerServer(&follower);
  if (!Expect(server.server != nullptr && server.port > 0,
              "peer server should start")) {
    return false;
  }

  std::optional<kvstore::raft::Message> vote_response;
  std::optional<kvstore::raft::Message> append_response;
  kvstore::raft::NetworkTransport transport(
      1, {{2, "127.0.0.1:" + std::to_string(server.port)}},
      [&vote_response, &append_response](kvstore::raft::Message message) {
        if (std::holds_alternative<kvstore::raft::RequestVoteResponse>(message.rpc)) {
          vote_response = std::move(message);
        } else if (std::holds_alternative<kvstore::raft::AppendEntriesResponse>(
                       message.rpc)) {
          append_response = std::move(message);
        }
      });

  transport.SetNodeUp(2, true);

  kvstore::raft::Message vote_message;
  vote_message.from = 1;
  vote_message.to = 2;
  vote_message.rpc = kvstore::raft::RequestVoteRequest{
      .term = 3,
      .candidate_id = 1,
      .last_log_index = 0,
      .last_log_term = 0,
  };
  transport.Send(vote_message);

  if (!Expect(vote_response.has_value(), "vote response should be received")) {
    return false;
  }
  const auto vote =
      std::get<kvstore::raft::RequestVoteResponse>(vote_response->rpc);
  if (!Expect(vote.vote_granted, "vote should be granted")) {
    return false;
  }

  kvstore::raft::Message append_message;
  append_message.from = 1;
  append_message.to = 2;
  append_message.rpc = kvstore::raft::AppendEntriesRequest{
      .term = 4,
      .leader_id = 1,
      .prev_log_index = 0,
      .prev_log_term = 0,
      .entries = {kvstore::raft::LogEntry{.term = 4, .command = "cmd-1"}},
      .leader_commit = 0,
  };
  transport.Send(append_message);

  if (!Expect(append_response.has_value(), "append response should be received")) {
    return false;
  }
  const auto append =
      std::get<kvstore::raft::AppendEntriesResponse>(append_response->rpc);
  if (!Expect(append.success, "append should succeed")) {
    return false;
  }
  const auto log_entry = follower.log_entry_at(1);
  if (!Expect(log_entry.has_value() && log_entry->command == "cmd-1",
              "follower log should receive replicated command")) {
    return false;
  }

  server.server->Shutdown();
  return true;
}

}  // namespace

int main() {
  if (!TestNetworkTransportRoundTrip()) {
    return 1;
  }
  return 0;
}
