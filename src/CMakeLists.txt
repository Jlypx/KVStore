add_library(
  kvstore_core
  smoke.cpp
)

target_include_directories(
  kvstore_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

kvstore_apply_compiler_warnings(kvstore_core)
kvstore_apply_sanitizers(kvstore_core)
