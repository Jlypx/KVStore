#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "kvstore/runtime/cluster_config.h"

namespace {

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_cluster_config_" + suffix + "_test");
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path, ec);
  return path;
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

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto ValidConfigText() -> std::string {
  return R"(cluster_id = "kvstore-local-1"
self_id = 1
tls_profile = "dev"
data_dir = "./data/node1"
client_addr = "127.0.0.1:50051"
peer_addr = "127.0.0.1:60051"

[[nodes]]
node_id = 1
client_addr = "127.0.0.1:50051"
peer_addr = "127.0.0.1:60051"

[[nodes]]
node_id = 2
client_addr = "127.0.0.1:50052"
peer_addr = "127.0.0.1:60052"

[[nodes]]
node_id = 3
client_addr = "127.0.0.1:50053"
peer_addr = "127.0.0.1:60053"

[[nodes]]
node_id = 4
client_addr = "127.0.0.1:50054"
peer_addr = "127.0.0.1:60054"

[[nodes]]
node_id = 5
client_addr = "127.0.0.1:50055"
peer_addr = "127.0.0.1:60055"
)";
}

auto TestLoadValidClusterConfig() -> bool {
  const auto dir = MakeTempDirectory("valid");
  const auto config_path = dir / "node1.toml";
  if (!Expect(WriteFile(config_path, ValidConfigText()), "config file should be written")) {
    return false;
  }

  kvstore::runtime::ClusterProcessConfig config;
  std::string error;
  if (!Expect(kvstore::runtime::LoadClusterProcessConfig(config_path, &config, &error),
              "valid config should load")) {
    std::cerr << error << '\n';
    return false;
  }

  return Expect(config.cluster_id == "kvstore-local-1", "cluster_id should load") &&
         Expect(config.self_id == 1, "self_id should load") &&
         Expect(config.tls_profile == "dev", "tls_profile should load") &&
         Expect(config.client_addr == "127.0.0.1:50051", "client_addr should load") &&
         Expect(config.peer_addr == "127.0.0.1:60051", "peer_addr should load") &&
         Expect(config.nodes.size() == 5, "five nodes should load");
}

auto TestRejectDuplicateNodeIds() -> bool {
  const auto dir = MakeTempDirectory("dup_id");
  const auto config_path = dir / "node_dup.toml";
  std::string text = ValidConfigText();
  text += "\n[[nodes]]\nnode_id = 5\nclient_addr = \"127.0.0.1:50056\"\npeer_addr = \"127.0.0.1:60056\"\n";
  if (!Expect(WriteFile(config_path, text), "duplicate id config file should be written")) {
    return false;
  }

  kvstore::runtime::ClusterProcessConfig config;
  std::string error;
  const bool ok =
      kvstore::runtime::LoadClusterProcessConfig(config_path, &config, &error);
  return Expect(!ok, "duplicate node ids must be rejected") &&
         Expect(!error.empty(), "duplicate node id error should be reported");
}

auto TestRejectMissingSelfId() -> bool {
  const auto dir = MakeTempDirectory("missing_self");
  const auto config_path = dir / "node_missing_self.toml";
  std::string text = ValidConfigText();
  const auto pos = text.find("self_id = 1");
  text.replace(pos, std::string("self_id = 1").size(), "self_id = 9");
  if (!Expect(WriteFile(config_path, text), "missing self config file should be written")) {
    return false;
  }

  kvstore::runtime::ClusterProcessConfig config;
  std::string error;
  const bool ok =
      kvstore::runtime::LoadClusterProcessConfig(config_path, &config, &error);
  return Expect(!ok, "missing self id must be rejected") &&
         Expect(!error.empty(), "missing self id error should be reported");
}

}  // namespace

int main() {
  if (!TestLoadValidClusterConfig()) {
    return 1;
  }
  if (!TestRejectDuplicateNodeIds()) {
    return 1;
  }
  if (!TestRejectMissingSelfId()) {
    return 1;
  }
  return 0;
}
