#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>

#include "kvstore/api/grpc_kv_service.h"
#include "kvstore/runtime/cluster_config.h"
#include "kvstore/service/cluster_node_service.h"
#include "kvstore/service/raft_peer_service.h"

namespace {

struct Args {
  std::string mode = "embedded";
  std::string listen_addr = "127.0.0.1:50051";
  std::filesystem::path data_dir = std::filesystem::path("./data");
  std::string tls_profile = "dev";
  std::filesystem::path tls_cert;
  std::filesystem::path tls_key;
  std::filesystem::path config_path;
};

auto StartsWith(const std::string& s, const std::string& prefix) -> bool {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

auto ReadAll(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

auto ReplacePort(const std::string& listen_addr, int selected_port) -> std::string {
  const auto port = std::to_string(selected_port);
  if (!listen_addr.empty() && listen_addr.front() == '[') {
    const auto bracket = listen_addr.rfind("]:");
    if (bracket != std::string::npos) {
      return listen_addr.substr(0, bracket + 2) + port;
    }
  }

  const auto colon = listen_addr.rfind(':');
  if (colon == std::string::npos) {
    return listen_addr;
  }
  return listen_addr.substr(0, colon + 1) + port;
}

auto ParseArgs(int argc, char** argv) -> Args {
  Args out;
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a == "--help" || a == "-h") {
      std::cout << "kvd (v1)\n\n"
                   "Flags:\n"
                   "  --mode=embedded|cluster-node (default embedded)\n"
                   "  --listen_addr=IP:PORT         (default 127.0.0.1:50051)\n"
                   "  --data_dir=PATH               (default ./data)\n"
                   "  --tls_profile=dev|secure      (default dev)\n"
                    "  --tls_cert=PATH               (required for --tls_profile=secure)\n"
                   "  --tls_key=PATH                (required for --tls_profile=secure)\n"
                   "  --config=PATH                 (required for --mode=cluster-node)\n";
      std::exit(0);
    }
    if (StartsWith(a, "--mode=")) {
      out.mode = a.substr(std::string("--mode=").size());
      continue;
    }
    if (StartsWith(a, "--listen_addr=")) {
      out.listen_addr = a.substr(std::string("--listen_addr=").size());
      continue;
    }
    if (StartsWith(a, "--data_dir=")) {
      out.data_dir = a.substr(std::string("--data_dir=").size());
      continue;
    }
    if (StartsWith(a, "--tls_profile=")) {
      out.tls_profile = a.substr(std::string("--tls_profile=").size());
      continue;
    }
    if (StartsWith(a, "--tls_cert=")) {
      out.tls_cert = a.substr(std::string("--tls_cert=").size());
      continue;
    }
    if (StartsWith(a, "--tls_key=")) {
      out.tls_key = a.substr(std::string("--tls_key=").size());
      continue;
    }
    if (StartsWith(a, "--config=")) {
      out.config_path = a.substr(std::string("--config=").size());
      continue;
    }
  }
  return out;
}

auto MakeCredentials(const std::string& tls_profile,
                     const std::filesystem::path& tls_cert,
                     const std::filesystem::path& tls_key)
    -> std::shared_ptr<grpc::ServerCredentials> {
  if (tls_profile == "dev") {
    return grpc::InsecureServerCredentials();
  }
  if (tls_profile == "secure") {
    if (tls_cert.empty() || tls_key.empty()) {
      return nullptr;
    }

    const auto cert_pem = ReadAll(tls_cert);
    const auto key_pem = ReadAll(tls_key);
    if (cert_pem.empty() || key_pem.empty()) {
      return nullptr;
    }

    grpc::SslServerCredentialsOptions::PemKeyCertPair key_cert = {key_pem, cert_pem};
    grpc::SslServerCredentialsOptions options;
    options.pem_key_cert_pairs.push_back(key_cert);
    return grpc::SslServerCredentials(options);
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = ParseArgs(argc, argv);

  if (args.mode == "embedded") {
    auto credentials = MakeCredentials(args.tls_profile, args.tls_cert, args.tls_key);
    if (!credentials) {
      std::cerr << "invalid embedded TLS configuration\n";
      return 1;
    }

    auto svc = std::make_shared<kvstore::service::KvRaftService>(
        args.data_dir, kvstore::service::RaftOptions{});
    kvstore::api::GrpcKvService grpc_service(svc);

    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort(args.listen_addr, credentials, &selected_port);
    builder.RegisterService(&grpc_service);

    auto server = builder.BuildAndStart();
    if (!server || selected_port <= 0) {
      std::cerr << "failed to start gRPC server on " << args.listen_addr << '\n';
      return 1;
    }
    const auto effective_listen_addr = ReplacePort(args.listen_addr, selected_port);
    std::cout << "kvd embedded listening on " << effective_listen_addr
              << " (requested=" << args.listen_addr
              << ", tls_profile=" << args.tls_profile
              << ", data_dir=" << args.data_dir.string() << ")\n";
    server->Wait();
    return 0;
  }

  if (args.mode != "cluster-node") {
    std::cerr << "unsupported --mode=" << args.mode
              << "; expected embedded or cluster-node\n";
    return 1;
  }

  if (args.config_path.empty()) {
    std::cerr << "--config=PATH is required for --mode=cluster-node\n";
    return 1;
  }

  kvstore::runtime::ClusterProcessConfig config;
  std::string config_error;
  if (!kvstore::runtime::LoadClusterProcessConfig(args.config_path, &config,
                                                  &config_error)) {
    std::cerr << "failed to load cluster-node config: " << config_error << '\n';
    return 1;
  }

  auto client_credentials =
      MakeCredentials(config.tls_profile, args.tls_cert, args.tls_key);
  if (!client_credentials) {
    std::cerr << "invalid cluster-node client TLS configuration\n";
    return 1;
  }

  auto svc = std::make_shared<kvstore::service::ClusterNodeService>(config);
  if (!svc->Start()) {
    std::cerr << "failed to start cluster node service for node " << config.self_id
              << '\n';
    return 1;
  }
  kvstore::api::GrpcKvService client_service(svc);
  kvstore::service::RaftPeerService peer_service(
      [svc](kvstore::raft::NodeId from,
            const kvstore::raft::RequestVoteRequest& request) {
        return svc->HandlePeerRequestVote(from, request);
      },
      [svc](kvstore::raft::NodeId from,
            const kvstore::raft::AppendEntriesRequest& request) {
        return svc->HandlePeerAppendEntries(from, request);
      });

  grpc::ServerBuilder client_builder;
  int client_port = 0;
  client_builder.AddListeningPort(config.client_addr, client_credentials, &client_port);
  client_builder.RegisterService(&client_service);

  grpc::ServerBuilder peer_builder;
  int peer_port = 0;
  peer_builder.AddListeningPort(config.peer_addr, grpc::InsecureServerCredentials(),
                                &peer_port);
  peer_builder.RegisterService(&peer_service);

  auto client_server = client_builder.BuildAndStart();
  auto peer_server = peer_builder.BuildAndStart();
  if (!client_server || !peer_server || client_port <= 0 || peer_port <= 0) {
    std::cerr << "failed to start cluster-node listeners for node " << config.self_id
              << '\n';
    return 1;
  }

  std::thread peer_waiter([&peer_server] { peer_server->Wait(); });
  std::cout << "kvd cluster-node started (node_id=" << config.self_id
            << ", client_addr=" << config.client_addr
            << ", peer_addr=" << config.peer_addr
            << ", data_dir=" << config.data_dir.string() << ")\n";
  client_server->Wait();
  peer_server->Shutdown();
  if (peer_waiter.joinable()) {
    peer_waiter.join();
  }
  return 0;
}
