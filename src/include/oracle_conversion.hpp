//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_conversion.hpp
//
// Helpers to convert ODPI-C data values into DuckDB column vectors.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "oracle_utils.hpp"
#include "duckdb/common/types/data_chunk.hpp"

namespace duckdb {

//! Convert a single ODPI-C data cell into a DuckDB Vector at the given row index.
//! type_info and oracle_type describe how to interpret the value.
void OracleConvertValue(Vector &col, idx_t out_row,
                        dpiData *data, dpiNativeTypeNum native_type,
                        const LogicalType &target_type,
                        const OracleType &oracle_type);

} // namespace duckdb
