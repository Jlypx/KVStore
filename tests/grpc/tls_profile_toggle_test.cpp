#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>

#include "kvstore/api/grpc_kv_service.h"
#include "kvstore/service/kv_raft_service.h"
#include "kvstore/v1/kv.grpc.pb.h"

namespace {

enum class Mode {
  kInProcess,
  kExternal,
};

struct Args {
  Mode mode = Mode::kInProcess;
  std::filesystem::path cert_path;
  std::filesystem::path key_path;
  std::string target;
  std::string tls_profile = "dev";
};

auto StartsWith(const std::string& s, const std::string& prefix) -> bool {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void PrintUsage() {
  std::cerr
      << "usage: tls_profile_toggle_test <cert_path> <key_path>\n"
      << "   or: tls_profile_toggle_test --mode=external --target=HOST:PORT "
         "--tls_profile=dev|secure [--tls_cert=PATH]\n";
}

auto ParseArgs(int argc, char** argv) -> std::optional<Args> {
  if (argc == 3 && !StartsWith(argv[1], "--") && !StartsWith(argv[2], "--")) {
    Args args;
    args.cert_path = argv[1];
    args.key_path = argv[2];
    return args;
  }

  Args args;
  bool saw_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a == "--help" || a == "-h") {
      PrintUsage();
      return std::nullopt;
    }
    if (StartsWith(a, "--mode=")) {
      const auto mode = a.substr(std::string("--mode=").size());
      if (mode != "external") {
        std::cerr << "unsupported --mode=" << mode << "; expected external\n";
        PrintUsage();
        return std::nullopt;
      }
      args.mode = Mode::kExternal;
      saw_mode = true;
      continue;
    }
    if (StartsWith(a, "--target=")) {
      args.target = a.substr(std::string("--target=").size());
      continue;
    }
    if (StartsWith(a, "--tls_profile=")) {
      args.tls_profile = a.substr(std::string("--tls_profile=").size());
      continue;
    }
    if (StartsWith(a, "--tls_cert=")) {
      args.cert_path = a.substr(std::string("--tls_cert=").size());
      continue;
    }
    if (StartsWith(a, "--tls_key=")) {
      args.key_path = a.substr(std::string("--tls_key=").size());
      continue;
    }
    std::cerr << "unrecognized argument: " << a << '\n';
    PrintUsage();
    return std::nullopt;
  }

  if (!saw_mode || args.mode != Mode::kExternal || args.target.empty()) {
    PrintUsage();
    return std::nullopt;
  }
  if (args.tls_profile != "dev" && args.tls_profile != "secure") {
    std::cerr << "unsupported --tls_profile=" << args.tls_profile
              << "; expected dev or secure\n";
    return std::nullopt;
  }
  if (args.tls_profile == "secure" && args.cert_path.empty()) {
    std::cerr << "secure external mode requires --tls_cert=PATH\n";
    return std::nullopt;
  }
  return args;
}

auto ReadAll(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

auto MakeTempDirectory(const std::string& suffix) -> std::filesystem::path {
  const auto path = std::filesystem::temp_directory_path() /
                    ("kvstore_task7_tls_profile_" + suffix);
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

struct ServerHandle {
  std::shared_ptr<kvstore::service::KvRaftService> core;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<kvstore::api::GrpcKvService> service;
  int port = 0;
};

auto StartDevServer(const std::filesystem::path& data_dir) -> ServerHandle {
  ServerHandle h;
  h.core = std::make_shared<kvstore::service::KvRaftService>(
      data_dir, kvstore::service::RaftOptions{});
  h.service = std::make_unique<kvstore::api::GrpcKvService>(h.core);

  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                           &h.port);
  builder.RegisterService(h.service.get());
  h.server = builder.BuildAndStart();
  return h;
}

auto StartSecureServer(const std::filesystem::path& data_dir,
                       const std::filesystem::path& cert_path,
                       const std::filesystem::path& key_path) -> ServerHandle {
  ServerHandle h;
  h.core = std::make_shared<kvstore::service::KvRaftService>(
      data_dir, kvstore::service::RaftOptions{});
  h.service = std::make_unique<kvstore::api::GrpcKvService>(h.core);

  grpc::SslServerCredentialsOptions::PemKeyCertPair key_cert = {
      ReadAll(key_path), ReadAll(cert_path)};
  grpc::SslServerCredentialsOptions options;
  options.pem_key_cert_pairs.push_back(key_cert);

  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(options),
                           &h.port);
  builder.RegisterService(h.service.get());
  h.server = builder.BuildAndStart();
  return h;
}

auto CheckPutGet(kvstore::v1::KV::Stub& stub,
                 const std::string& key,
                 const std::string& value,
                 const std::string& request_id) -> bool {
  kvstore::v1::PutRequest put_req;
  put_req.set_key(key);
  put_req.set_value(value);
  put_req.set_request_id(request_id);
  kvstore::v1::PutResponse put_resp;
  grpc::ClientContext put_ctx;
  if (!stub.Put(&put_ctx, put_req, &put_resp).ok()) {
    return false;
  }

  kvstore::v1::GetRequest get_req;
  get_req.set_key(key);
  kvstore::v1::GetResponse get_resp;
  grpc::ClientContext get_ctx;
  const auto get_status = stub.Get(&get_ctx, get_req, &get_resp);
  return get_status.ok() && get_resp.found() && get_resp.value() == value;
}

auto CheckPutGetWithRetry(kvstore::v1::KV::Stub& stub,
                          const std::string& key,
                          const std::string& value,
                          const std::string& request_id,
                          std::chrono::milliseconds timeout,
                          std::chrono::milliseconds retry_interval) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (CheckPutGet(stub, key, value, request_id)) {
      return true;
    }
    std::this_thread::sleep_for(retry_interval);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

auto MakeSecureStub(const std::string& target,
                    const std::filesystem::path& cert_path)
    -> std::unique_ptr<kvstore::v1::KV::Stub> {
  grpc::SslCredentialsOptions ssl_options;
  ssl_options.pem_root_certs = ReadAll(cert_path);
  if (ssl_options.pem_root_certs.empty()) {
    return nullptr;
  }

  grpc::ChannelArguments channel_args;
  channel_args.SetSslTargetNameOverride("localhost");
  return kvstore::v1::KV::NewStub(grpc::CreateCustomChannel(
      target, grpc::SslCredentials(ssl_options), channel_args));
}

auto MakeExternalStub(const Args& args) -> std::unique_ptr<kvstore::v1::KV::Stub> {
  std::shared_ptr<grpc::ChannelCredentials> credentials;
  if (args.tls_profile == "dev") {
    credentials = grpc::InsecureChannelCredentials();
  } else {
    return MakeSecureStub(args.target, args.cert_path);
  }
  return kvstore::v1::KV::NewStub(grpc::CreateChannel(args.target, credentials));
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = ParseArgs(argc, argv);
  if (!args.has_value()) {
    return 2;
  }

  if (args->mode == Mode::kExternal) {
    auto stub = MakeExternalStub(*args);
    if (!Expect(stub != nullptr, "external client credentials should load")) {
      return 1;
    }
    if (!Expect(CheckPutGetWithRetry(*stub, "tls-k", "external-" + args->tls_profile + "-v",
                                     "tls-external-" + args->tls_profile + "-req",
                                     std::chrono::seconds(10),
                                     std::chrono::milliseconds(200)),
                "external profile Put/Get semantics")) {
      return 1;
    }
    std::cout << "{\"pass\":true,\"mode\":\"external\",\"target\":\""
              << args->target << "\",\"tls_profile\":\"" << args->tls_profile
              << "\"}" << '\n';
    return 0;
  }

  const std::filesystem::path cert_path = args->cert_path;
  const std::filesystem::path key_path = args->key_path;

  auto dev = StartDevServer(MakeTempDirectory("dev"));
  if (!Expect(dev.server != nullptr && dev.port > 0, "dev server should start")) {
    return 1;
  }
  auto dev_channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(dev.port),
                                         grpc::InsecureChannelCredentials());
  auto dev_stub = kvstore::v1::KV::NewStub(dev_channel);
  if (!Expect(CheckPutGet(*dev_stub, "tls-k", "dev-v", "tls-dev-req"),
              "dev profile Put/Get semantics")) {
    return 1;
  }
  dev.server->Shutdown();

  auto secure =
      StartSecureServer(MakeTempDirectory("secure"), cert_path, key_path);
  if (!Expect(secure.server != nullptr && secure.port > 0,
              "secure server should start")) {
    return 1;
  }
  auto secure_stub = MakeSecureStub("127.0.0.1:" + std::to_string(secure.port), cert_path);
  if (!Expect(secure_stub != nullptr, "secure client credentials should load")) {
    return 1;
  }
  if (!Expect(CheckPutGet(*secure_stub, "tls-k", "secure-v", "tls-secure-req"),
              "secure profile Put/Get semantics")) {
    return 1;
  }
  secure.server->Shutdown();

  std::cout << "{\"pass\":true,\"profiles\":[\"dev\",\"secure\"],"
               "\"semantic_drift\":false}"
            << '\n';
  return 0;
}
