#ifndef KVSTORE_RAFT_RAFT_STORAGE_H
#define KVSTORE_RAFT_RAFT_STORAGE_H

#include <filesystem>
#include <vector>

#include "kvstore/raft/raft_types.h"

namespace kvstore::raft {

struct PersistentRaftState {
  Term current_term = 0;
  NodeId voted_for = kNoVote;
  LogIndex snapshot_last_included_index = 0;
  Term snapshot_last_included_term = 0;
  std::vector<LogEntry> log;
};

class RaftStorage {
 public:
  explicit RaftStorage(std::filesystem::path dir);

  auto Load(PersistentRaftState* out) -> bool;
  auto StoreMetadata(Term term, NodeId voted_for,
                     LogIndex snapshot_last_included_index = 0,
                     Term snapshot_last_included_term = 0) -> bool;
  auto StoreLog(const std::vector<LogEntry>& log) -> bool;

 private:
  auto EnsureDirectory() -> bool;
  auto meta_path() const -> std::filesystem::path;
  auto log_path() const -> std::filesystem::path;

  std::filesystem::path dir_;
};

}  // namespace kvstore::raft

#endif  // KVSTORE_RAFT_RAFT_STORAGE_H
