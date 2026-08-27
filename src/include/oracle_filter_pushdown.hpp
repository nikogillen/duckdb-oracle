//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_filter_pushdown.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "oracle_utils.hpp"

namespace duckdb {

class OracleFilterPushdown {
public:
	//! Transform DuckDB table filters into an Oracle WHERE clause fragment.
	//! `oracle_types` and `duck_types` are indexed like `names` and decide which
	//! columns may carry a pushed filter at all - Oracle rejects LOBs as comparison
	//! keys, and the wrapped types (JSON, VECTOR, SDO_GEOMETRY) are not comparable
	//! against a plain literal either. Columns without usable type information are
	//! left to DuckDB. Returns an empty string when nothing can be pushed.
	static string TransformFilters(const vector<column_t> &column_ids,
	                               optional_ptr<TableFilterSet> filters,
	                               const vector<string> &names,
	                               const vector<OracleType> &oracle_types,
	                               const vector<LogicalType> &duck_types);
};

} // namespace duckdb
