#include "kvstore/raft/network_transport.h"

#include <grpcpp/grpcpp.h>

#include "kvstore/raft/raft_proto_conversion.h"
#include "kvstore/v1/raft.grpc.pb.h"

namespace kvstore::raft {

NetworkTransport::NetworkTransport(
    NodeId local_node_id,
    std::unordered_map<NodeId, std::string> peer_addrs,
    MessageHandler receive_handler)
    : local_node_id_(local_node_id),
      peer_addrs_(std::move(peer_addrs)),
      receive_handler_(std::move(receive_handler)) {}

auto NetworkTransport::RegisterNode(NodeId id, MessageHandler handler) -> void {
  if (id == local_node_id_) {
    receive_handler_ = std::move(handler);
  }
}

auto NetworkTransport::SetNodeUp(NodeId id, bool up) -> void { up_[id] = up; }

auto NetworkTransport::IsNodeUp(NodeId id) const -> bool {
  const auto it = up_.find(id);
  if (it == up_.end()) {
    return true;
  }
  return it->second;
}

auto NetworkTransport::GetOrCreateStub(NodeId id) -> kvstore::v1::RaftPeer::Stub* {
  const auto found = stubs_.find(id);
  if (found != stubs_.end()) {
    return found->second.get();
  }
  const auto addr_it = peer_addrs_.find(id);
  if (addr_it == peer_addrs_.end()) {
    return nullptr;
  }
  auto channel = grpc::CreateChannel(addr_it->second, grpc::InsecureChannelCredentials());
  auto stub = kvstore::v1::RaftPeer::NewStub(channel);
  auto* out = stub.get();
  stubs_.emplace(id, std::move(stub));
  return out;
}

auto NetworkTransport::Send(Message message) -> void {
  if (!IsNodeUp(message.to)) {
    return;
  }
  auto* stub = GetOrCreateStub(message.to);
  if (stub == nullptr || !receive_handler_) {
    return;
  }

  if (std::holds_alternative<RequestVoteRequest>(message.rpc)) {
    grpc::ClientContext context;
    const auto request = ToProto(std::get<RequestVoteRequest>(message.rpc));
    kvstore::v1::RequestVoteResponse response;
    const auto status = stub->RequestVote(&context, request, &response);
    if (!status.ok()) {
      return;
    }
    receive_handler_(Message{
        .from = message.to,
        .to = local_node_id_,
        .rpc = Rpc{FromProto(response)},
    });
    return;
  }

  if (std::holds_alternative<AppendEntriesRequest>(message.rpc)) {
    grpc::ClientContext context;
    const auto request = ToProto(std::get<AppendEntriesRequest>(message.rpc));
    kvstore::v1::AppendEntriesResponse response;
    const auto status = stub->AppendEntries(&context, request, &response);
    if (!status.ok()) {
      return;
    }
    receive_handler_(Message{
        .from = message.to,
        .to = local_node_id_,
        .rpc = Rpc{FromProto(response)},
    });
  }
}

}  // namespace kvstore::raft
