//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_result.hpp
//
// Materialized query result for schema introspection queries.
// For large data scans, use OracleReader (streaming) instead.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "oracle_utils.hpp"

namespace duckdb {

struct OracleResultCell {
	string value;
	bool is_null = true;
};

class OracleResult {
public:
	vector<vector<OracleResultCell>> rows;
	uint64_t affected_rows = 0;

public:
	string GetString(idx_t row, idx_t col) const {
		D_ASSERT(row < rows.size());
		D_ASSERT(col < rows[row].size());
		return rows[row][col].value;
	}

	string_t GetStringRef(idx_t row, idx_t col) const {
		D_ASSERT(row < rows.size());
		D_ASSERT(col < rows[row].size());
		const auto &cell = rows[row][col];
		return string_t(cell.value.c_str(), cell.value.size());
	}

	int32_t GetInt32(idx_t row, idx_t col) const {
		return atoi(GetString(row, col).c_str());
	}

	int64_t GetInt64(idx_t row, idx_t col) const {
		return atoll(GetString(row, col).c_str());
	}

	bool GetBool(idx_t row, idx_t col) const {
		auto s = GetString(row, col);
		return s == "Y" || s == "y" || s == "1" || s == "true" || s == "TRUE";
	}

	bool IsNull(idx_t row, idx_t col) const {
		D_ASSERT(row < rows.size());
		D_ASSERT(col < rows[row].size());
		return rows[row][col].is_null;
	}

	idx_t Count() const {
		return rows.size();
	}

	idx_t AffectedRows() const {
		return (idx_t)affected_rows;
	}
};

} // namespace duckdb
