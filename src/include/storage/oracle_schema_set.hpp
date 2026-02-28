//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_schema_set.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/oracle_catalog_set.hpp"

namespace duckdb {

class OracleSchemaSet : public OracleCatalogSet {
public:
	OracleSchemaSet(OracleCatalog &catalog, const string &schema_to_load);

protected:
	void LoadEntries(ClientContext &context, OracleTransaction &transaction) override;

private:
	string schema_to_load;
};

} // namespace duckdb
