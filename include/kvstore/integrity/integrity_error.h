#ifndef KVSTORE_INTEGRITY_INTEGRITY_ERROR_H
#define KVSTORE_INTEGRITY_INTEGRITY_ERROR_H

#include <cstddef>
#include <optional>
#include <string>

namespace kvstore::integrity {

enum class IntegrityErrorCode {
  kIoError,
  kInvalidMagic,
  kUnsupportedVersion,
  kInvalidOperation,
  kTruncatedRecord,
  kChecksumMismatch,
  kInvalidRecord,
};

struct IntegrityError {
  IntegrityErrorCode code = IntegrityErrorCode::kIoError;
  std::size_t record_index = 0;
  std::string message;
};

inline auto ToString(IntegrityErrorCode code) -> std::string {
  switch (code) {
    case IntegrityErrorCode::kIoError:
      return "IO_ERROR";
    case IntegrityErrorCode::kInvalidMagic:
      return "INVALID_MAGIC";
    case IntegrityErrorCode::kUnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case IntegrityErrorCode::kInvalidOperation:
      return "INVALID_OPERATION";
    case IntegrityErrorCode::kTruncatedRecord:
      return "TRUNCATED_RECORD";
    case IntegrityErrorCode::kChecksumMismatch:
      return "CHECKSUM_MISMATCH";
    case IntegrityErrorCode::kInvalidRecord:
      return "INVALID_RECORD";
  }
  return "UNKNOWN_INTEGRITY_ERROR";
}

inline auto FormatIntegrityLogLine(const IntegrityError& error) -> std::string {
  return "integrity_code=" + ToString(error.code) + " record_index=" +
         std::to_string(error.record_index) + " message=\"" + error.message +
         "\"";
}

}  // namespace kvstore::integrity

#endif  // KVSTORE_INTEGRITY_INTEGRITY_ERROR_H
