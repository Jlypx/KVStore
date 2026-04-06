#ifndef KVSTORE_RUNTIME_CLUSTER_CONFIG_H
#define KVSTORE_RUNTIME_CLUSTER_CONFIG_H

#include <filesystem>
#include <string>
#include <vector>

#include "kvstore/raft/raft_types.h"

namespace kvstore::runtime {

struct ClusterNodeConfig {
  kvstore::raft::NodeId node_id = 0;
  std::string client_addr;
  std::string peer_addr;
};

struct ClusterProcessConfig {
  std::string cluster_id;
  kvstore::raft::NodeId self_id = 0;
  std::string tls_profile;
  std::filesystem::path data_dir;
  std::string client_addr;
  std::string peer_addr;
  std::vector<ClusterNodeConfig> nodes;
};

auto LoadClusterProcessConfig(const std::filesystem::path& path,
                              ClusterProcessConfig* out,
                              std::string* error) -> bool;

}  // namespace kvstore::runtime

#endif  // KVSTORE_RUNTIME_CLUSTER_CONFIG_H
