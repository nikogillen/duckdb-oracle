//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_type_entry.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "oracle_utils.hpp"

namespace duckdb {

class OracleTypeEntry : public TypeCatalogEntry {
public:
	OracleTypeEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTypeInfo &info);

	OracleType oracle_type;
};

} // namespace duckdb
