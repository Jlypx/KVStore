#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>

#include "kvstore/api/grpc_kv_service.h"

namespace {

struct Args {
  std::string listen_addr = "127.0.0.1:50051";
  std::filesystem::path data_dir = std::filesystem::path("./data");
  std::string tls_profile = "dev";
  std::filesystem::path tls_cert;
  std::filesystem::path tls_key;
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
                   "  --listen_addr=IP:PORT         (default 127.0.0.1:50051)\n"
                   "  --data_dir=PATH               (default ./data)\n"
                   "  --tls_profile=dev|secure      (default dev)\n"
                   "  --tls_cert=PATH               (required for --tls_profile=secure)\n"
                   "  --tls_key=PATH                (required for --tls_profile=secure)\n";
      std::exit(0);
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
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = ParseArgs(argc, argv);

  std::shared_ptr<grpc::ServerCredentials> credentials;
  if (args.tls_profile == "dev") {
    credentials = grpc::InsecureServerCredentials();
  } else if (args.tls_profile == "secure") {
    if (args.tls_cert.empty() || args.tls_key.empty()) {
      std::cerr << "secure TLS profile requires both --tls_cert=PATH and --tls_key=PATH\n";
      return 1;
    }

    const auto cert_pem = ReadAll(args.tls_cert);
    if (cert_pem.empty()) {
      std::cerr << "failed to read TLS certificate PEM from " << args.tls_cert.string()
                << '\n';
      return 1;
    }

    const auto key_pem = ReadAll(args.tls_key);
    if (key_pem.empty()) {
      std::cerr << "failed to read TLS private key PEM from " << args.tls_key.string()
                << '\n';
      return 1;
    }

    grpc::SslServerCredentialsOptions::PemKeyCertPair key_cert = {key_pem, cert_pem};
    grpc::SslServerCredentialsOptions options;
    options.pem_key_cert_pairs.push_back(key_cert);
    credentials = grpc::SslServerCredentials(options);
  } else {
    std::cerr << "unsupported --tls_profile=" << args.tls_profile
              << "; expected dev or secure\n";
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
  std::cout << "kvd listening on " << effective_listen_addr << " (requested="
            << args.listen_addr << ", tls_profile=" << args.tls_profile
            << ", data_dir=" << args.data_dir.string() << ")\n";
  server->Wait();
  return 0;
}
