//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_index_set.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/oracle_catalog_set.hpp"

namespace duckdb {

class OracleIndexSet : public OracleInSchemaSet {
public:
	explicit OracleIndexSet(OracleSchemaEntry &schema, unique_ptr<OracleResultSlice> index_result = nullptr);

protected:
	void LoadEntries(ClientContext &context, OracleTransaction &transaction) override;

private:
	unique_ptr<OracleResultSlice> index_result;
};

} // namespace duckdb
