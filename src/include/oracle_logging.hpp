//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_logging.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logging.hpp"

namespace duckdb {

class OracleQueryLogType : public LogType {
public:
	OracleQueryLogType();
	static constexpr const char *NAME = "oracle_query";
};

} // namespace duckdb
