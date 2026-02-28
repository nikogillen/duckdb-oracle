#include "storage/oracle_type_set.hpp"
#include "storage/oracle_transaction.hpp"

namespace duckdb {

OracleTypeSet::OracleTypeSet(OracleSchemaEntry &schema)
    : OracleInSchemaSet(schema, true) { // mark loaded = true so we don't query
}

void OracleTypeSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	// Oracle does not have user-defined enum types (prior to 23c domains).
	// This set stays empty.
}

} // namespace duckdb
