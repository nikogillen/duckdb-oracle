//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_storage.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class OracleStorageExtension : public StorageExtension {
public:
	OracleStorageExtension();
};

} // namespace duckdb
