add_executable(kvstore_grpc_integration_test grpc_integration_test.cpp)
target_link_libraries(kvstore_grpc_integration_test PRIVATE kvstore_api)
if(NOT MSVC)
  target_link_options(kvstore_grpc_integration_test PRIVATE -Wl,--disable-new-dtags)
endif()
kvstore_apply_compiler_warnings(kvstore_grpc_integration_test)
kvstore_apply_sanitizers(kvstore_grpc_integration_test)
add_test(NAME kvstore_grpc_integration_test COMMAND kvstore_grpc_integration_test)

add_executable(kvstore_grpc_idempotency_test grpc_idempotency_test.cpp)
target_link_libraries(kvstore_grpc_idempotency_test PRIVATE kvstore_api)
if(NOT MSVC)
  target_link_options(kvstore_grpc_idempotency_test PRIVATE -Wl,--disable-new-dtags)
endif()
kvstore_apply_compiler_warnings(kvstore_grpc_idempotency_test)
kvstore_apply_sanitizers(kvstore_grpc_idempotency_test)
add_test(NAME kvstore_grpc_idempotency_test COMMAND kvstore_grpc_idempotency_test)

add_executable(kvstore_api_status_test api_status_test.cpp)
target_link_libraries(kvstore_api_status_test PRIVATE kvstore_api)
if(NOT MSVC)
  target_link_options(kvstore_api_status_test PRIVATE -Wl,--disable-new-dtags)
endif()
kvstore_apply_compiler_warnings(kvstore_api_status_test)
kvstore_apply_sanitizers(kvstore_api_status_test)
add_test(NAME kvstore_api_status_test COMMAND kvstore_api_status_test)

add_executable(kvstore_tls_profile_toggle_test tls_profile_toggle_test.cpp)
target_link_libraries(kvstore_tls_profile_toggle_test PRIVATE kvstore_api)
if(NOT MSVC)
  target_link_options(kvstore_tls_profile_toggle_test PRIVATE -Wl,--disable-new-dtags)
endif()
kvstore_apply_compiler_warnings(kvstore_tls_profile_toggle_test)
kvstore_apply_sanitizers(kvstore_tls_profile_toggle_test)

# Ensure gRPC-linked tests can locate repo-local runtime.
set(_grpc_rpath
  "${PROJECT_SOURCE_DIR}/.tools/grpc/sysroot/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
  "${PROJECT_SOURCE_DIR}/.tools/grpc/sysroot/usr/lib"
)
set_property(TARGET kvstore_grpc_integration_test PROPERTY BUILD_RPATH "${_grpc_rpath}")
set_property(TARGET kvstore_grpc_idempotency_test PROPERTY BUILD_RPATH "${_grpc_rpath}")
set_property(TARGET kvstore_api_status_test PROPERTY BUILD_RPATH "${_grpc_rpath}")
set_property(TARGET kvstore_tls_profile_toggle_test PROPERTY BUILD_RPATH "${_grpc_rpath}")

set(_grpc_test_env
  "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/.tools/grpc/sysroot/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}:${PROJECT_SOURCE_DIR}/.tools/grpc/sysroot/usr/lib:$ENV{LD_LIBRARY_PATH}"
)
set_tests_properties(
  kvstore_grpc_integration_test
  kvstore_grpc_idempotency_test
  kvstore_api_status_test
  PROPERTIES ENVIRONMENT "${_grpc_test_env}"
)

add_test(
  NAME kvstore_tls_profile_toggle_test
  COMMAND kvstore_tls_profile_toggle_test
    ${PROJECT_SOURCE_DIR}/tests/assets/tls/server.crt
    ${PROJECT_SOURCE_DIR}/tests/assets/tls/server.key
)
set_tests_properties(
  kvstore_tls_profile_toggle_test
  PROPERTIES ENVIRONMENT "${_grpc_test_env}"
)
