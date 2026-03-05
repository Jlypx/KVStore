#include <iostream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "kvstore/api/grpc_kv_service.h"
#include "kvstore/service/kv_raft_service.h"

namespace {

auto Expect(bool condition, const std::string& message) -> bool {
  if (!condition) {
    std::cerr << "ASSERTION FAILED: " << message << '\n';
    return false;
  }
  return true;
}

auto TestErrorStatusMapping() -> bool {
  using kvstore::service::Error;
  using kvstore::service::ErrorCode;

  {
    const auto st = kvstore::api::ToGrpcStatus(
        Error{.code = ErrorCode::kInvalidArgument, .message = "bad request"});
    if (!Expect(st.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
                "invalid argument should map to INVALID_ARGUMENT")) {
      return false;
    }
  }

  {
    const auto st = kvstore::api::ToGrpcStatus(
        Error{.code = ErrorCode::kNotLeader,
              .message = "not leader",
              .leader_hint = 3});
    if (!Expect(st.error_code() == grpc::StatusCode::FAILED_PRECONDITION,
                "not leader should map to FAILED_PRECONDITION") ||
        !Expect(st.error_message().find("leader_hint=3") != std::string::npos,
                "not leader status should carry leader_hint")) {
      return false;
    }
  }

  {
    const auto st = kvstore::api::ToGrpcStatus(
        Error{.code = ErrorCode::kUnavailable, .message = "quorum unavailable"});
    if (!Expect(st.error_code() == grpc::StatusCode::UNAVAILABLE,
                "unavailable should map to UNAVAILABLE")) {
      return false;
    }
  }

  {
    const auto st = kvstore::api::ToGrpcStatus(
        Error{.code = ErrorCode::kTimeout, .message = "timed out"});
    if (!Expect(st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED,
                "timeout should map to DEADLINE_EXCEEDED")) {
      return false;
    }
  }

  return true;
}

}  // namespace

int main() {
  if (!TestErrorStatusMapping()) {
    return 1;
  }
  return 0;
}
