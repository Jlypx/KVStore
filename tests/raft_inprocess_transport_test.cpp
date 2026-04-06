#include <iostream>
#include <optional>
#include <string>

#include "kvstore/raft/inprocess_transport.h"

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto TestInProcessTransportDeliversToRegisteredHandler() -> bool {
  kvstore::raft::InProcessTransport transport;
  std::optional<kvstore::raft::Message> delivered;

  transport.RegisterNode(2, [&delivered](kvstore::raft::Message message) {
    delivered = std::move(message);
  });
  transport.SetNodeUp(1, true);
  transport.SetNodeUp(2, true);

  kvstore::raft::Message message;
  message.from = 1;
  message.to = 2;
  message.rpc = kvstore::raft::RequestVoteRequest{
      .term = 3,
      .candidate_id = 1,
      .last_log_index = 5,
      .last_log_term = 2,
  };

  transport.Send(message);
  const auto delivered_count = transport.DeliverAll();

  return Expect(delivered_count == 1, "one message should be delivered") &&
         Expect(delivered.has_value(), "handler should receive message") &&
         Expect(delivered->from == 1, "delivered from should match") &&
         Expect(delivered->to == 2, "delivered to should match");
}

auto TestInProcessTransportSkipsDownNodes() -> bool {
  kvstore::raft::InProcessTransport transport;
  bool called = false;

  transport.RegisterNode(2, [&called](kvstore::raft::Message) { called = true; });
  transport.SetNodeUp(1, true);
  transport.SetNodeUp(2, false);

  kvstore::raft::Message message;
  message.from = 1;
  message.to = 2;
  message.rpc = kvstore::raft::AppendEntriesResponse{
      .term = 4,
      .success = true,
      .match_index = 9,
  };

  transport.Send(message);
  const auto delivered_count = transport.DeliverAll();

  return Expect(delivered_count == 0, "no message should be delivered to down node") &&
         Expect(!called, "down node handler should not be called");
}

}  // namespace

int main() {
  if (!TestInProcessTransportDeliversToRegisteredHandler()) {
    return 1;
  }
  if (!TestInProcessTransportSkipsDownNodes()) {
    return 1;
  }
  return 0;
}
