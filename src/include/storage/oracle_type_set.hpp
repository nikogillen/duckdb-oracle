//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_type_set.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/oracle_catalog_set.hpp"
#include "storage/oracle_type_entry.hpp"

namespace duckdb {

// Oracle does not have user-defined enum types (prior to 23c domains).
// This set is kept empty but present so the schema entry compiles correctly.
class OracleTypeSet : public OracleInSchemaSet {
public:
	explicit OracleTypeSet(OracleSchemaEntry &schema);

protected:
	void LoadEntries(ClientContext &context, OracleTransaction &transaction) override;
};

} // namespace duckdb
