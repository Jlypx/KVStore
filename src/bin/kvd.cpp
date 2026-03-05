#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kvstore/api/grpc_kv_service.h"

namespace {

struct Args {
  std::string listen_addr = "127.0.0.1:50051";
  std::filesystem::path data_dir = std::filesystem::path("./data");
};

auto StartsWith(const std::string& s, const std::string& prefix) -> bool {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

auto ParseArgs(int argc, char** argv) -> Args {
  Args out;
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a == "--help" || a == "-h") {
      std::cout << "kvd (v1)\n\n"
                   "Flags:\n"
                   "  --listen_addr=IP:PORT   (default 127.0.0.1:50051)\n"
                   "  --data_dir=PATH         (default ./data)\n";
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
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = ParseArgs(argc, argv);

  auto svc = std::make_shared<kvstore::service::KvRaftService>(
      args.data_dir, kvstore::service::RaftOptions{});
  kvstore::api::GrpcKvService grpc_service(svc);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(args.listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&grpc_service);

  auto server = builder.BuildAndStart();
  if (!server) {
    std::cerr << "failed to start gRPC server on " << args.listen_addr << '\n';
    return 1;
  }
  std::cout << "kvd listening on " << args.listen_addr << " (data_dir="
            << args.data_dir.string() << ")\n";
  server->Wait();
  return 0;
}
