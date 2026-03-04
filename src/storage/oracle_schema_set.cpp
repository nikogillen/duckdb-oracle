#include "storage/oracle_schema_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

namespace duckdb {

OracleSchemaSet::OracleSchemaSet(OracleCatalog &catalog, const string &schema_to_load)
    : OracleCatalogSet(catalog), schema_to_load(schema_to_load) {
}

void OracleSchemaSet::LoadEntries(ClientContext &context, OracleTransaction &transaction) {
	string query;
	if (!schema_to_load.empty()) {
		// Load only the requested schema
		query = StringUtil::Format(
		    "SELECT DISTINCT owner FROM all_tables WHERE owner = %s "
		    "UNION SELECT DISTINCT owner FROM all_views WHERE owner = %s",
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)),
		    OracleUtils::WriteLiteral(StringUtil::Upper(schema_to_load)));
	} else {
		// Load all visible schemas/owners
		query =
		    "SELECT DISTINCT owner FROM all_tables "
		    "UNION SELECT DISTINCT owner FROM all_views "
		    "ORDER BY 1";
	}

	auto result = transaction.Query(query);
	if (!result) {
		return;
	}

	for (idx_t row = 0; row < result->Count(); row++) {
		auto owner = result->GetString(row, 0);
		CreateSchemaInfo info;
		info.schema = owner;
		info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		auto entry = make_shared_ptr<OracleSchemaEntry>(catalog, info);
		entries[owner] = std::move(entry);
	}

	// Inject a stub "main" schema so that DuckDB UI's hardcoded
	// SET schema = '<catalog>.main' succeeds without a Catalog Error.
	// The stub's table/index/type sets are pre-loaded and empty, so every
	// lookup inside "main" returns nullptr immediately (no Oracle query).
	// DuckDB then falls through to memory.main for its own internal objects
	// (e.g. the "config" table used by the UI extension).
	if (entries.find("main") == entries.end()) {
		CreateSchemaInfo stub_info;
		stub_info.schema = "main";
		stub_info.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
		entries["main"] = make_shared_ptr<OracleSchemaEntry>(catalog, stub_info);
	}
}

} // namespace duckdb
