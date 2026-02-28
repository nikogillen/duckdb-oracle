#include "oracle_logging.hpp"

namespace duckdb {

OracleQueryLogType::OracleQueryLogType() : LogType(NAME, LogLevel::LOG_INFO) {
}

} // namespace duckdb
