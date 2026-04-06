#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kvstore/engine/kv_engine.h"
#include "kvstore/raft/raft_storage.h"
#include "kvstore/v1/kv.grpc.pb.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kSourceDir = KVSTORE_SOURCE_DIR;
constexpr const char* kBinaryDir = KVSTORE_BINARY_DIR;

struct NodeEndpoint {
  int node_id = 0;
  std::string client_addr;
  std::string peer_addr;
};

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       Clock::now().time_since_epoch())
                       .count();
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_cluster_mp_" + suffix + "_" + std::to_string(now));
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
}

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto AcquirePort() -> int {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return 0;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(sock);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(sock);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(sock);
  return port;
}

auto WriteFile(const std::filesystem::path& path, const std::string& content) -> bool {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << content;
  out.flush();
  return out.good();
}

auto MakeConfigText(const NodeEndpoint& self,
                    const std::filesystem::path& data_dir,
                    const std::vector<NodeEndpoint>& nodes) -> std::string {
  std::string text;
  text += "cluster_id = \"kvstore-local-test\"\n";
  text += "self_id = " + std::to_string(self.node_id) + "\n";
  text += "tls_profile = \"dev\"\n";
  text += "data_dir = \"" + data_dir.string() + "\"\n";
  text += "client_addr = \"" + self.client_addr + "\"\n";
  text += "peer_addr = \"" + self.peer_addr + "\"\n\n";
  for (const auto& node : nodes) {
    text += "[[nodes]]\n";
    text += "node_id = " + std::to_string(node.node_id) + "\n";
    text += "client_addr = \"" + node.client_addr + "\"\n";
    text += "peer_addr = \"" + node.peer_addr + "\"\n\n";
  }
  return text;
}

auto ParseLeaderHint(const std::string& message) -> std::optional<int> {
  const std::regex pattern(R"(leader_hint=(\d+))");
  std::smatch match;
  if (std::regex_search(message, match, pattern) && match.size() == 2) {
    return std::stoi(match[1].str());
  }
  return std::nullopt;
}

struct ClusterGuard {
  std::filesystem::path run_dir;
  ~ClusterGuard() {
    if (run_dir.empty()) {
      return;
    }
    const auto stop_script =
        std::filesystem::path(kSourceDir) / "scripts/cluster/stop_local_cluster.sh";
    const auto command = "bash \"" + stop_script.string() + "\" --run-dir \"" +
                         run_dir.string() + "\"";
    std::system(command.c_str());
  }
};

auto StopCluster(const std::filesystem::path& run_dir) -> void {
  const auto stop_script =
      std::filesystem::path(kSourceDir) / "scripts/cluster/stop_local_cluster.sh";
  const auto command = "bash \"" + stop_script.string() + "\" --run-dir \"" +
                       run_dir.string() + "\"";
  std::system(command.c_str());
}

auto GetStub(const std::string& addr) -> std::unique_ptr<kvstore::v1::KV::Stub> {
  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
  return kvstore::v1::KV::NewStub(channel);
}

auto FindLeaderEndpoint(const std::vector<NodeEndpoint>& nodes,
                        std::chrono::milliseconds timeout,
                        std::optional<int> exclude = std::nullopt)
    -> std::optional<NodeEndpoint> {
  const auto deadline = Clock::now() + timeout;
  int probe_index = 0;
  while (Clock::now() < deadline) {
    for (const auto& node : nodes) {
      if (exclude.has_value() && node.node_id == *exclude) {
        continue;
      }
      auto stub = GetStub(node.client_addr);
      kvstore::v1::GetRequest req;
      req.set_key("leader-probe");
      kvstore::v1::GetResponse resp;
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(200));
      const auto status = stub->Get(&ctx, req, &resp);
      if (status.ok()) {
        return node;
      }
      if (status.error_code() == grpc::StatusCode::FAILED_PRECONDITION) {
        const auto hint = ParseLeaderHint(status.error_message());
        if (hint.has_value()) {
          for (const auto& candidate : nodes) {
            if (candidate.node_id == *hint &&
                (!exclude.has_value() || candidate.node_id != *exclude)) {
              return candidate;
            }
          }
        }
      }
    }
    probe_index += 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return std::nullopt;
}

auto PutExpectStatus(const NodeEndpoint& node,
                     const std::string& key,
                     const std::string& value,
                     const std::string& request_id,
                     grpc::StatusCode code,
                     std::chrono::milliseconds timeout = std::chrono::seconds(2)) -> bool {
  auto stub = GetStub(node.client_addr);
  kvstore::v1::PutRequest req;
  req.set_key(key);
  req.set_value(value);
  req.set_request_id(request_id);
  kvstore::v1::PutResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + timeout);
  const auto status = stub->Put(&ctx, req, &resp);
  if (!status.ok() && code == grpc::StatusCode::OK) {
    std::cerr << "put error addr=" << node.client_addr
              << " code=" << static_cast<int>(status.error_code())
              << " message=" << status.error_message() << '\n';
  }
  if (code == grpc::StatusCode::OK) {
    return status.ok();
  }
  return !status.ok() && status.error_code() == code;
}

auto GetValue(const NodeEndpoint& node, const std::string& key)
    -> std::optional<std::string> {
  auto stub = GetStub(node.client_addr);
  kvstore::v1::GetRequest req;
  req.set_key(key);
  kvstore::v1::GetResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
  const auto status = stub->Get(&ctx, req, &resp);
  if (!status.ok() || !resp.found()) {
    return std::nullopt;
  }
  return resp.value();
}

auto KillNode(const std::filesystem::path& run_dir, int node_id) -> bool {
  const auto pid_path = run_dir / "pids" / ("node" + std::to_string(node_id) + ".pid");
  std::ifstream in(pid_path);
  if (!in.is_open()) {
    return false;
  }
  int pid = 0;
  in >> pid;
  return pid > 0 && ::kill(pid, SIGTERM) == 0;
}

auto StartNode(const std::filesystem::path& run_dir,
               const std::filesystem::path& config_path,
               int node_id) -> bool {
  const auto log_path = run_dir / "logs" / ("node" + std::to_string(node_id) + ".log");
  const auto pid_path = run_dir / "pids" / ("node" + std::to_string(node_id) + ".pid");
  const auto command = "\"" + std::string(kBinaryDir) + "/src/kvd\" --mode=cluster-node --config=\"" +
                       config_path.string() + "\" >\"" + log_path.string() + "\" 2>&1 & echo $! >\"" +
                       pid_path.string() + "\"";
  if (std::system(("bash -lc '" + command + "'").c_str()) != 0) {
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::ifstream in(pid_path);
  int pid = 0;
  in >> pid;
  return pid > 0;
}

auto TestMultiProcessCluster() -> bool {
  ClusterGuard guard;
  guard.run_dir = MakeTempDirectory("run");
  const auto config_dir = guard.run_dir / "configs";
  const auto data_root = guard.run_dir / "data";
  std::filesystem::create_directories(config_dir);
  std::filesystem::create_directories(data_root);

  std::vector<NodeEndpoint> nodes;
  nodes.reserve(5);
  for (int node_id = 1; node_id <= 5; ++node_id) {
    const int client_port = AcquirePort();
    const int peer_port = AcquirePort();
    if (!Expect(client_port > 0 && peer_port > 0, "ports should be allocated")) {
      return false;
    }
    nodes.push_back(NodeEndpoint{
        .node_id = node_id,
        .client_addr = "127.0.0.1:" + std::to_string(client_port),
        .peer_addr = "127.0.0.1:" + std::to_string(peer_port),
    });
  }

  for (const auto& node : nodes) {
    const auto config_path =
        config_dir / ("kvd_cluster_node_" + std::to_string(node.node_id) + ".toml");
    const auto data_dir = data_root / ("node" + std::to_string(node.node_id));
    if (!Expect(WriteFile(config_path, MakeConfigText(node, data_dir, nodes)),
                "cluster config should be written")) {
      return false;
    }
  }

  const auto start_script =
      std::filesystem::path(kSourceDir) / "scripts/cluster/start_local_cluster.sh";
  const auto command = "bash \"" + start_script.string() + "\" --build-dir \"" +
                       std::string(kBinaryDir) + "\" --run-dir \"" +
                       guard.run_dir.string() + "\" --config-dir \"" +
                       config_dir.string() + "\"";
  if (!Expect(std::system(command.c_str()) == 0, "cluster should start")) {
    return false;
  }

  const auto leader_before = FindLeaderEndpoint(nodes, std::chrono::seconds(10));
  if (!Expect(leader_before.has_value(), "leader should be elected")) {
    return false;
  }

  std::optional<NodeEndpoint> follower;
  for (const auto& node : nodes) {
    if (node.node_id != leader_before->node_id) {
      follower = node;
      break;
    }
  }
  if (!Expect(follower.has_value(), "a follower endpoint should exist")) {
    return false;
  }

  if (!Expect(PutExpectStatus(*follower, "cluster-k", "v-follower", "req-follower",
                              grpc::StatusCode::FAILED_PRECONDITION),
              "follower write should be rejected")) {
    return false;
  }

  if (!Expect(PutExpectStatus(*leader_before, "cluster-k", "v1", "req-leader-1",
                              grpc::StatusCode::OK),
              "leader write should succeed")) {
    return false;
  }
  const auto stable_leader = FindLeaderEndpoint(nodes, std::chrono::seconds(5));
  if (!Expect(stable_leader.has_value(),
              "leader should remain discoverable after first write")) {
    return false;
  }
  const auto before_value = GetValue(*stable_leader, "cluster-k");
  if (!Expect(before_value.has_value() && *before_value == "v1",
              "leader read should return first value")) {
    return false;
  }

  if (!Expect(KillNode(guard.run_dir, stable_leader->node_id), "leader should be killed")) {
    return false;
  }

  const auto leader_after = FindLeaderEndpoint(
      nodes, std::chrono::seconds(15), stable_leader->node_id);
  if (!Expect(leader_after.has_value(), "new leader should be elected")) {
    return false;
  }
  if (!Expect(leader_after->node_id != stable_leader->node_id,
              "new leader should differ from old leader")) {
    return false;
  }

  if (!Expect(PutExpectStatus(*leader_after, "cluster-k", "v2", "req-leader-2",
                              grpc::StatusCode::OK),
              "new leader write should succeed")) {
    return false;
  }
  const auto after_value = GetValue(*leader_after, "cluster-k");
  if (!Expect(after_value.has_value() && *after_value == "v2",
              "new leader read should return updated value")) {
    return false;
  }

  return true;
}

auto TestMultiProcessSnapshotCatchup() -> bool {
  ClusterGuard guard;
  guard.run_dir = MakeTempDirectory("snapshot");
  const auto config_dir = guard.run_dir / "configs";
  const auto data_root = guard.run_dir / "data";
  std::filesystem::create_directories(config_dir);
  std::filesystem::create_directories(data_root);

  std::vector<NodeEndpoint> nodes;
  nodes.reserve(5);
  for (int node_id = 1; node_id <= 5; ++node_id) {
    const int client_port = AcquirePort();
    const int peer_port = AcquirePort();
    if (!Expect(client_port > 0 && peer_port > 0, "ports should be allocated")) {
      return false;
    }
    nodes.push_back(NodeEndpoint{
        .node_id = node_id,
        .client_addr = "127.0.0.1:" + std::to_string(client_port),
        .peer_addr = "127.0.0.1:" + std::to_string(peer_port),
    });
  }

  for (const auto& node : nodes) {
    const auto config_path =
        config_dir / ("kvd_cluster_node_" + std::to_string(node.node_id) + ".toml");
    const auto data_dir = data_root / ("node" + std::to_string(node.node_id));
    if (!Expect(WriteFile(config_path, MakeConfigText(node, data_dir, nodes)),
                "snapshot cluster config should be written")) {
      return false;
    }
  }

  const auto start_script =
      std::filesystem::path(kSourceDir) / "scripts/cluster/start_local_cluster.sh";
  const auto command = "bash \"" + start_script.string() + "\" --build-dir \"" +
                       std::string(kBinaryDir) + "\" --run-dir \"" +
                       guard.run_dir.string() + "\" --config-dir \"" +
                       config_dir.string() + "\"";
  if (!Expect(std::system(command.c_str()) == 0, "snapshot cluster should start")) {
    return false;
  }

  const auto leader = FindLeaderEndpoint(nodes, std::chrono::seconds(10));
  if (!Expect(leader.has_value(), "snapshot cluster leader should be elected")) {
    return false;
  }

  std::optional<NodeEndpoint> lagging_follower;
  for (const auto& node : nodes) {
    if (node.node_id != leader->node_id) {
      lagging_follower = node;
      break;
    }
  }
  if (!Expect(lagging_follower.has_value(), "lagging follower should be chosen")) {
    return false;
  }

  if (!Expect(KillNode(guard.run_dir, lagging_follower->node_id),
              "lagging follower should be stopped")) {
    return false;
  }

  NodeEndpoint current_leader = *leader;
  for (int i = 0; i < 40; ++i) {
    bool wrote = false;
    for (int attempt = 0; attempt < 5 && !wrote; ++attempt) {
      wrote = PutExpectStatus(current_leader,
                              "snap-k-" + std::to_string(i),
                              "snap-v-" + std::to_string(i),
                              "snap-req-" + std::to_string(i),
                              grpc::StatusCode::OK,
                              std::chrono::seconds(5));
      if (!wrote) {
        const auto refreshed =
            FindLeaderEndpoint(nodes, std::chrono::seconds(5), lagging_follower->node_id);
        if (refreshed.has_value()) {
          current_leader = *refreshed;
        }
      }
    }
    if (!Expect(wrote,
                "leader writes during snapshot catch-up preparation should succeed")) {
      std::cerr << "snapshot multi-process write failed at i=" << i << '\n';
      return false;
    }
  }

  const auto follower_config =
      config_dir / ("kvd_cluster_node_" + std::to_string(lagging_follower->node_id) + ".toml");
  if (!Expect(StartNode(guard.run_dir, follower_config, lagging_follower->node_id),
              "lagging follower should restart")) {
    return false;
  }

  std::this_thread::sleep_for(std::chrono::seconds(6));
  StopCluster(guard.run_dir);
  guard.run_dir.clear();

  const auto follower_raft_dir =
      data_root / ("node" + std::to_string(lagging_follower->node_id)) / "raft";
  kvstore::raft::PersistentRaftState follower_state;
  bool loaded = false;
  for (int attempt = 0; attempt < 20 && !loaded; ++attempt) {
    kvstore::raft::RaftStorage follower_storage(follower_raft_dir);
    loaded = follower_storage.Load(&follower_state);
    if (!loaded) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!Expect(loaded, "follower raft storage should load after restart")) {
    return false;
  }
  if (!Expect(follower_state.snapshot_last_included_index > 0,
              "follower should record snapshot metadata after catch-up")) {
    return false;
  }

  const auto follower_wal =
      data_root / ("node" + std::to_string(lagging_follower->node_id)) / "engine" /
      "000001.wal";
  kvstore::engine::KvEngine follower_engine(follower_wal);
  if (!Expect(follower_engine.Open(), "follower engine should reopen after snapshot")) {
    return false;
  }
  const auto follower_value = follower_engine.Get("snap-k-20");
  return Expect(follower_value.has_value() && *follower_value == "snap-v-20",
                "lagging follower should recover snapshot-installed state");
}

}  // namespace

int main() {
  if (!TestMultiProcessCluster()) {
    return 1;
  }
  if (!TestMultiProcessSnapshotCatchup()) {
    return 1;
  }
  return 0;
}
