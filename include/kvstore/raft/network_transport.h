#ifndef KVSTORE_RAFT_NETWORK_TRANSPORT_H
#define KVSTORE_RAFT_NETWORK_TRANSPORT_H

#include <memory>
#include <string>
#include <unordered_map>

#include "kvstore/raft/raft_transport.h"
#include "kvstore/v1/raft.grpc.pb.h"

namespace kvstore::raft {

class NetworkTransport final : public RaftTransport {
 public:
  NetworkTransport(NodeId local_node_id,
                   std::unordered_map<NodeId, std::string> peer_addrs,
                   MessageHandler receive_handler);

  auto RegisterNode(NodeId id, MessageHandler handler) -> void override;
  auto SetNodeUp(NodeId id, bool up) -> void override;
  [[nodiscard]] auto IsNodeUp(NodeId id) const -> bool override;
  auto Send(Message message) -> void override;

 private:
  using StubPtr = std::unique_ptr<kvstore::v1::RaftPeer::Stub>;

  auto GetOrCreateStub(NodeId id) -> kvstore::v1::RaftPeer::Stub*;

  NodeId local_node_id_;
  std::unordered_map<NodeId, std::string> peer_addrs_;
  MessageHandler receive_handler_;
  std::unordered_map<NodeId, bool> up_;
  std::unordered_map<NodeId, StubPtr> stubs_;
};

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_NETWORK_TRANSPORT_H
