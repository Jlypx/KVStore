#ifndef KVSTORE_RAFT_RAFT_TRANSPORT_H
#define KVSTORE_RAFT_RAFT_TRANSPORT_H

#include <functional>

#include "kvstore/raft/raft_types.h"

namespace kvstore::raft {

using MessageHandler = std::function<void(Message)>;

class RaftTransport {
 public:
  virtual ~RaftTransport() = default;

  virtual auto RegisterNode(NodeId id, MessageHandler handler) -> void = 0;
  virtual auto SetNodeUp(NodeId id, bool up) -> void = 0;
  [[nodiscard]] virtual auto IsNodeUp(NodeId id) const -> bool = 0;
  virtual auto Send(Message message) -> void = 0;
};

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_RAFT_TRANSPORT_H
