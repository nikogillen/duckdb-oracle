//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_index.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/index_type_set.hpp"

namespace duckdb {

struct OracleIndex {
	static IndexStorageInfo GetStorageInfo(IndexStorageInfo &info);
	static void Serialize(Serializer &serializer, const IndexStorageInfo &info);
	static IndexStorageInfo Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
