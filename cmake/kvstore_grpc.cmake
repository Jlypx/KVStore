include_guard(GLOBAL)

set(KVSTORE_GRPC_SYSROOT "${PROJECT_SOURCE_DIR}/.tools/grpc/sysroot/usr")
set(KVSTORE_GRPC_BOOTSTRAP_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/ci/bootstrap_grpc_runtime.sh")

function(kvstore_bootstrap_grpc_runtime)
  if(EXISTS "${KVSTORE_GRPC_SYSROOT}/include/grpcpp/grpcpp.h" AND
     EXISTS "${KVSTORE_GRPC_SYSROOT}/include/google/protobuf/message.h")
    return()
  endif()

  if(NOT EXISTS "${KVSTORE_GRPC_BOOTSTRAP_SCRIPT}")
    message(FATAL_ERROR "Missing gRPC bootstrap script: ${KVSTORE_GRPC_BOOTSTRAP_SCRIPT}")
  endif()

  execute_process(
    COMMAND bash "${KVSTORE_GRPC_BOOTSTRAP_SCRIPT}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE _grpc_bootstrap_rc
    OUTPUT_VARIABLE _grpc_bootstrap_out
    ERROR_VARIABLE _grpc_bootstrap_err
  )
  if(NOT _grpc_bootstrap_rc EQUAL 0)
    message(FATAL_ERROR "gRPC runtime bootstrap failed (rc=${_grpc_bootstrap_rc})\n${_grpc_bootstrap_out}\n${_grpc_bootstrap_err}")
  endif()
endfunction()

function(kvstore_define_grpc_runtime_target)
  kvstore_bootstrap_grpc_runtime()
  find_package(Threads REQUIRED)

  # Library search paths within extracted sysroot.
  set(_grpc_lib_paths
    "${KVSTORE_GRPC_SYSROOT}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
    "${KVSTORE_GRPC_SYSROOT}/lib"
  )

  find_library(KVSTORE_PROTOBUF_LIB NAMES protobuf PATHS ${_grpc_lib_paths} NO_DEFAULT_PATH)
  find_library(KVSTORE_GRPC_LIB NAMES grpc PATHS ${_grpc_lib_paths} NO_DEFAULT_PATH)
  find_library(KVSTORE_GRPCPP_LIB NAMES grpc++ PATHS ${_grpc_lib_paths} NO_DEFAULT_PATH)
  find_library(KVSTORE_GPR_LIB NAMES gpr PATHS ${_grpc_lib_paths} NO_DEFAULT_PATH)

  if(NOT KVSTORE_PROTOBUF_LIB OR NOT KVSTORE_GRPC_LIB OR NOT KVSTORE_GRPCPP_LIB OR NOT KVSTORE_GPR_LIB)
    message(FATAL_ERROR "Unable to locate repo-local gRPC/protobuf libraries under ${KVSTORE_GRPC_SYSROOT}. Missing: protobuf=${KVSTORE_PROTOBUF_LIB}, grpc=${KVSTORE_GRPC_LIB}, grpc++=${KVSTORE_GRPCPP_LIB}, gpr=${KVSTORE_GPR_LIB}")
  endif()

  if(NOT TARGET kvstore_grpc_runtime)
    add_library(kvstore_grpc_runtime INTERFACE)
    target_include_directories(kvstore_grpc_runtime SYSTEM INTERFACE
      "${KVSTORE_GRPC_SYSROOT}/include"
    )
    target_link_libraries(kvstore_grpc_runtime INTERFACE
      "${KVSTORE_GRPCPP_LIB}"
      "${KVSTORE_GRPC_LIB}"
      "${KVSTORE_GPR_LIB}"
      "${KVSTORE_PROTOBUF_LIB}"
      Threads::Threads
    )
  endif()
endfunction()

function(kvstore_bootstrap_proto_tools)
  set(_local_sysroot "${PROJECT_SOURCE_DIR}/.tools/proto/sysroot")
  set(_local_protoc "${_local_sysroot}/usr/bin/protoc")
  set(_local_plugin "${_local_sysroot}/usr/bin/grpc_cpp_plugin")
  if(EXISTS "${_local_protoc}" AND EXISTS "${_local_plugin}")
    set(KVSTORE_PROTOC_BIN "${_local_protoc}" PARENT_SCOPE)
    set(KVSTORE_GRPC_CPP_PLUGIN_BIN "${_local_plugin}" PARENT_SCOPE)
    return()
  endif()

  execute_process(
    COMMAND bash "${PROJECT_SOURCE_DIR}/scripts/ci/bootstrap_proto_tools.sh"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    RESULT_VARIABLE _proto_bootstrap_rc
    OUTPUT_VARIABLE _proto_bootstrap_out
    ERROR_VARIABLE _proto_bootstrap_err
  )
  if(NOT _proto_bootstrap_rc EQUAL 0)
    message(FATAL_ERROR "Proto tools bootstrap failed (rc=${_proto_bootstrap_rc})\n${_proto_bootstrap_out}\n${_proto_bootstrap_err}")
  endif()

  if(NOT EXISTS "${_local_protoc}" OR NOT EXISTS "${_local_plugin}")
    message(FATAL_ERROR "Proto tools bootstrap succeeded but protoc/grpc_cpp_plugin not found under ${_local_sysroot}")
  endif()

  set(KVSTORE_PROTOC_BIN "${_local_protoc}" PARENT_SCOPE)
  set(KVSTORE_GRPC_CPP_PLUGIN_BIN "${_local_plugin}" PARENT_SCOPE)
endfunction()
