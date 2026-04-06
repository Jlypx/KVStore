add_library(
  kvstore_core
  smoke.cpp
  raft/raft_node.cpp
  raft/raft_storage.cpp
  engine/memtable.cpp
  engine/wal.cpp
  engine/kv_engine.cpp
  engine/sstable.cpp
  engine/compaction.cpp
  cache/block_cache.cpp
  integrity/crc32c.cpp
)

target_include_directories(
  kvstore_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

kvstore_apply_compiler_warnings(kvstore_core)
kvstore_apply_sanitizers(kvstore_core)

if(KVSTORE_ENABLE_GRPC)
  include(kvstore_grpc)
  kvstore_define_grpc_runtime_target()
  kvstore_bootstrap_proto_tools()

  set(KVSTORE_KV_PROTO "${PROJECT_SOURCE_DIR}/proto/kvstore/v1/kv.proto")
  set(KVSTORE_GEN_DIR "${PROJECT_BINARY_DIR}/gen")

  file(MAKE_DIRECTORY "${KVSTORE_GEN_DIR}")
  file(MAKE_DIRECTORY "${KVSTORE_GEN_DIR}/kvstore/v1")

  # Ensure repo-local protoc can find its shared libs.
  set(_proto_sysroot_usr "${PROJECT_SOURCE_DIR}/.tools/proto/sysroot/usr")
  set(_proto_ld_dirs
    "${_proto_sysroot_usr}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
    "${_proto_sysroot_usr}/lib"
  )
  string(JOIN ":" _proto_ld_path ${_proto_ld_dirs})

  add_custom_command(
    OUTPUT
      "${KVSTORE_GEN_DIR}/kvstore/v1/kv.pb.h"
      "${KVSTORE_GEN_DIR}/kvstore/v1/kv.pb.cc"
      "${KVSTORE_GEN_DIR}/kvstore/v1/kv.grpc.pb.h"
      "${KVSTORE_GEN_DIR}/kvstore/v1/kv.grpc.pb.cc"
    COMMAND ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=${_proto_ld_path}:$ENV{LD_LIBRARY_PATH}"
      "${KVSTORE_PROTOC_BIN}"
      --proto_path=${PROJECT_SOURCE_DIR}/proto
      --cpp_out=${KVSTORE_GEN_DIR}
      --grpc_out=${KVSTORE_GEN_DIR}
      --plugin=protoc-gen-grpc=${KVSTORE_GRPC_CPP_PLUGIN_BIN}
      "${KVSTORE_KV_PROTO}"
    DEPENDS "${KVSTORE_KV_PROTO}"
    COMMENT "Generating C++ protobuf + gRPC stubs for kv.proto"
    VERBATIM
  )

  add_library(kvstore_kv_proto
    "${KVSTORE_GEN_DIR}/kvstore/v1/kv.pb.cc"
    "${KVSTORE_GEN_DIR}/kvstore/v1/kv.grpc.pb.cc"
  )
  target_include_directories(kvstore_kv_proto PUBLIC
    "${KVSTORE_GEN_DIR}"
  )
  target_link_libraries(kvstore_kv_proto PUBLIC kvstore_grpc_runtime)
  # Generated code: keep build green even under -Werror.
  if(NOT MSVC)
    target_compile_options(kvstore_kv_proto PRIVATE -Wno-unused-parameter)
  endif()

  add_library(kvstore_service
    raft/cluster_runtime.cpp
    service/kv_raft_service.cpp
    raft/test_transport.cpp
  )
  find_package(Threads REQUIRED)
  target_link_libraries(kvstore_service PUBLIC kvstore_core Threads::Threads)
  target_include_directories(kvstore_service PUBLIC ${PROJECT_SOURCE_DIR}/include)
  kvstore_apply_compiler_warnings(kvstore_service)
  kvstore_apply_sanitizers(kvstore_service)

  add_library(kvstore_api
    api/grpc_kv_service.cpp
  )
  target_link_libraries(kvstore_api PUBLIC kvstore_service kvstore_kv_proto kvstore_grpc_runtime)
  target_include_directories(kvstore_api PUBLIC ${PROJECT_SOURCE_DIR}/include)
  kvstore_apply_compiler_warnings(kvstore_api)
  kvstore_apply_sanitizers(kvstore_api)

  add_executable(kvd
    bin/kvd.cpp
  )
  target_link_libraries(kvd PRIVATE kvstore_api)
  if(NOT MSVC)
    target_link_options(kvd PRIVATE -Wl,--disable-new-dtags)
  endif()
  kvstore_apply_compiler_warnings(kvd)
  kvstore_apply_sanitizers(kvd)

  # Ensure executables can locate repo-local gRPC/protobuf runtime.
  set(_grpc_rpath
    "${KVSTORE_GRPC_SYSROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
    "${KVSTORE_GRPC_SYSROOT}/lib"
  )
  set_property(TARGET kvstore_api PROPERTY BUILD_RPATH "${_grpc_rpath}")
  set_property(TARGET kvd PROPERTY BUILD_RPATH "${_grpc_rpath}")
endif()
