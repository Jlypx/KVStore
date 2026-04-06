#include "kvstore/raft/inprocess_transport.h"

#include <utility>

namespace kvstore::raft {

auto InProcessTransport::RegisterNode(NodeId id, MessageHandler handler) -> void {
  handlers_[id] = std::move(handler);
  if (up_.find(id) == up_.end()) {
    up_[id] = true;
  }
}

auto InProcessTransport::SetNodeUp(NodeId id, bool up) -> void { up_[id] = up; }

auto InProcessTransport::IsNodeUp(NodeId id) const -> bool {
  const auto it = up_.find(id);
  if (it == up_.end()) {
    return false;
  }
  return it->second;
}

auto InProcessTransport::Send(Message message) -> void {
  if (!IsNodeUp(message.from) || !IsNodeUp(message.to)) {
    return;
  }

  queue_.push_back(Envelope{.seq = next_seq_, .message = std::move(message)});
  next_seq_ += 1;
}

auto InProcessTransport::DeliverSome(std::size_t limit) -> std::size_t {
  std::size_t delivered = 0;
  while (!queue_.empty() && delivered < limit) {
    Envelope envelope = std::move(queue_.front());
    queue_.pop_front();

    if (!IsNodeUp(envelope.message.to)) {
      continue;
    }
    const auto it = handlers_.find(envelope.message.to);
    if (it == handlers_.end() || !it->second) {
      continue;
    }

    it->second(std::move(envelope.message));
    delivered += 1;
  }
  return delivered;
}

auto InProcessTransport::DeliverAll() -> std::size_t {
  constexpr std::size_t kMax = 1'000'000;
  return DeliverSome(kMax);
}

}  // namespace kvstore::raft
