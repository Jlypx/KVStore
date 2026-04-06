#ifndef KVSTORE_RAFT_INPROCESS_TRANSPORT_H
#define KVSTORE_RAFT_INPROCESS_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>

#include "kvstore/raft/raft_transport.h"

namespace kvstore::raft {

class InProcessTransport : public RaftTransport {
 public:
  auto RegisterNode(NodeId id, MessageHandler handler) -> void override;

  auto SetNodeUp(NodeId id, bool up) -> void override;
  [[nodiscard]] auto IsNodeUp(NodeId id) const -> bool override;

  auto Send(Message message) -> void override;

  auto DeliverSome(std::size_t limit) -> std::size_t;
  auto DeliverAll() -> std::size_t;

  [[nodiscard]] auto pending_message_count() const -> std::size_t {
    return queue_.size();
  }

 private:
  struct Envelope {
    std::uint64_t seq = 0;
    Message message;
  };

  std::uint64_t next_seq_ = 1;
  std::deque<Envelope> queue_;
  std::unordered_map<NodeId, MessageHandler> handlers_;
  std::unordered_map<NodeId, bool> up_;
};

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_INPROCESS_TRANSPORT_H
