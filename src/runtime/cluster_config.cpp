#include "kvstore/runtime/cluster_config.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace kvstore::runtime {
namespace {

auto Trim(std::string_view input) -> std::string {
  std::size_t start = 0;
  while (start < input.size() &&
         std::isspace(static_cast<unsigned char>(input[start])) != 0) {
    start += 1;
  }
  std::size_t end = input.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    end -= 1;
  }
  return std::string(input.substr(start, end - start));
}

auto ParseQuotedString(const std::string& value, std::string* out) -> bool {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return false;
  }
  *out = value.substr(1, value.size() - 2);
  return true;
}

auto ParseU32(const std::string& value, kvstore::raft::NodeId* out) -> bool {
  if (value.empty()) {
    return false;
  }
  std::uint64_t parsed = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    parsed = parsed * 10ULL + static_cast<std::uint64_t>(ch - '0');
  }
  *out = static_cast<kvstore::raft::NodeId>(parsed);
  return true;
}

auto SetProcessField(ClusterProcessConfig* config,
                     const std::string& key,
                     const std::string& value,
                     std::string* error) -> bool {
  if (key == "cluster_id") {
    return ParseQuotedString(value, &config->cluster_id);
  }
  if (key == "self_id") {
    return ParseU32(value, &config->self_id);
  }
  if (key == "tls_profile") {
    return ParseQuotedString(value, &config->tls_profile);
  }
  if (key == "data_dir") {
    std::string parsed;
    if (!ParseQuotedString(value, &parsed)) {
      return false;
    }
    config->data_dir = parsed;
    return true;
  }
  if (key == "client_addr") {
    return ParseQuotedString(value, &config->client_addr);
  }
  if (key == "peer_addr") {
    return ParseQuotedString(value, &config->peer_addr);
  }
  if (error != nullptr) {
    *error = "unknown process config key: " + key;
  }
  return false;
}

auto SetNodeField(ClusterNodeConfig* node,
                  const std::string& key,
                  const std::string& value,
                  std::string* error) -> bool {
  if (key == "node_id") {
    return ParseU32(value, &node->node_id);
  }
  if (key == "client_addr") {
    return ParseQuotedString(value, &node->client_addr);
  }
  if (key == "peer_addr") {
    return ParseQuotedString(value, &node->peer_addr);
  }
  if (error != nullptr) {
    *error = "unknown node config key: " + key;
  }
  return false;
}

}  // namespace

auto LoadClusterProcessConfig(const std::filesystem::path& path,
                              ClusterProcessConfig* out,
                              std::string* error) -> bool {
  if (out == nullptr) {
    if (error != nullptr) {
      *error = "output config pointer is null";
    }
    return false;
  }

  std::ifstream input(path);
  if (!input.is_open()) {
    if (error != nullptr) {
      *error = "failed to open config file";
    }
    return false;
  }

  *out = ClusterProcessConfig{};
  ClusterNodeConfig current_node;
  bool in_node_block = false;
  auto flush_current_node = [&]() {
    if (in_node_block) {
      out->nodes.push_back(current_node);
      current_node = ClusterNodeConfig{};
      in_node_block = false;
    }
  };

  std::string line;
  std::size_t line_no = 0;
  while (std::getline(input, line)) {
    line_no += 1;
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (trimmed == "[[nodes]]") {
      flush_current_node();
      in_node_block = true;
      continue;
    }

    const auto eq = trimmed.find('=');
    if (eq == std::string::npos) {
      if (error != nullptr) {
        *error = "invalid config line " + std::to_string(line_no);
      }
      return false;
    }

    const auto key = Trim(trimmed.substr(0, eq));
    const auto value = Trim(trimmed.substr(eq + 1));
    const bool ok = in_node_block
                        ? SetNodeField(&current_node, key, value, error)
                        : SetProcessField(out, key, value, error);
    if (!ok) {
      if (error != nullptr && error->empty()) {
        *error = "invalid value for key '" + key + "' on line " +
                 std::to_string(line_no);
      }
      return false;
    }
  }
  flush_current_node();

  if (out->cluster_id.empty() || out->self_id == 0 || out->tls_profile.empty() ||
      out->data_dir.empty() || out->client_addr.empty() || out->peer_addr.empty()) {
    if (error != nullptr) {
      *error = "missing required process-level fields";
    }
    return false;
  }
  if (out->nodes.size() != 5U) {
    if (error != nullptr) {
      *error = "cluster config must define exactly five nodes";
    }
    return false;
  }

  std::unordered_set<kvstore::raft::NodeId> node_ids;
  std::unordered_set<std::string> client_addrs;
  std::unordered_set<std::string> peer_addrs;
  bool self_found = false;
  for (const auto& node : out->nodes) {
    if (node.node_id == 0 || node.client_addr.empty() || node.peer_addr.empty()) {
      if (error != nullptr) {
        *error = "node entry missing required fields";
      }
      return false;
    }
    if (!node_ids.insert(node.node_id).second) {
      if (error != nullptr) {
        *error = "duplicate node_id in cluster config";
      }
      return false;
    }
    if (!client_addrs.insert(node.client_addr).second) {
      if (error != nullptr) {
        *error = "duplicate client_addr in cluster config";
      }
      return false;
    }
    if (!peer_addrs.insert(node.peer_addr).second) {
      if (error != nullptr) {
        *error = "duplicate peer_addr in cluster config";
      }
      return false;
    }
    if (node.client_addr == node.peer_addr) {
      if (error != nullptr) {
        *error = "node client_addr and peer_addr must differ";
      }
      return false;
    }
    if (node.node_id == out->self_id) {
      self_found = true;
    }
  }

  if (!self_found) {
    if (error != nullptr) {
      *error = "self_id is not present in cluster member list";
    }
    return false;
  }

  return true;
}

}  // namespace kvstore::runtime
