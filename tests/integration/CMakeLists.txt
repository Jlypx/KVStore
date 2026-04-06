find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(kvstore_integration_failover_test failover_test.cpp)
target_link_libraries(kvstore_integration_failover_test PRIVATE kvstore_service)
kvstore_apply_compiler_warnings(kvstore_integration_failover_test)
kvstore_apply_sanitizers(kvstore_integration_failover_test)
add_test(NAME kvstore_integration_failover_test COMMAND kvstore_integration_failover_test)

add_executable(kvstore_chaos_gate_test chaos_gate_test.cpp)
target_link_libraries(kvstore_chaos_gate_test PRIVATE kvstore_service)
kvstore_apply_compiler_warnings(kvstore_chaos_gate_test)
kvstore_apply_sanitizers(kvstore_chaos_gate_test)
add_test(NAME kvstore_chaos_gate_test COMMAND kvstore_chaos_gate_test failover)

add_executable(kvstore_integrity_gate_test integrity_gate_test.cpp)
target_link_libraries(kvstore_integrity_gate_test PRIVATE kvstore_core)
kvstore_apply_compiler_warnings(kvstore_integrity_gate_test)
kvstore_apply_sanitizers(kvstore_integrity_gate_test)
add_test(
  NAME kvstore_integrity_gate_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/integrity/run_corruption_suite.py
          --repo-root ${CMAKE_SOURCE_DIR}
          --build-dir ${CMAKE_BINARY_DIR}
)

add_executable(kvstore_bench_gate_test bench_gate_test.cpp)
target_link_libraries(kvstore_bench_gate_test PRIVATE kvstore_service)
kvstore_apply_compiler_warnings(kvstore_bench_gate_test)
kvstore_apply_sanitizers(kvstore_bench_gate_test)
add_test(NAME kvstore_bench_gate_test COMMAND kvstore_bench_gate_test)

add_executable(kvstore_multi_process_cluster_test multi_process_cluster_test.cpp)
target_link_libraries(kvstore_multi_process_cluster_test PRIVATE kvstore_kv_proto kvstore_grpc_runtime)
if(NOT MSVC)
  target_link_options(kvstore_multi_process_cluster_test PRIVATE -Wl,--disable-new-dtags)
endif()
target_compile_definitions(kvstore_multi_process_cluster_test PRIVATE
  KVSTORE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  KVSTORE_BINARY_DIR="${PROJECT_BINARY_DIR}"
)
kvstore_apply_compiler_warnings(kvstore_multi_process_cluster_test)
kvstore_apply_sanitizers(kvstore_multi_process_cluster_test)
add_test(NAME kvstore_multi_process_cluster_test COMMAND kvstore_multi_process_cluster_test)

set(_grpc_rpath
  "${PROJECT_SOURCE_DIR}/.tools/grpc/sysroot/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
  "${PROJECT_SOURCE_DIR}/.tools/grpc/sysroot/usr/lib"
)
set_property(TARGET kvstore_multi_process_cluster_test PROPERTY BUILD_RPATH "${_grpc_rpath}")
