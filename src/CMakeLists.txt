add_library(
  kvstore_core
  smoke.cpp
  raft/raft_node.cpp
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
