#include "storage/oracle_index_entry.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

OracleIndexEntry::OracleIndexEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                    CreateIndexInfo &info)
    : IndexCatalogEntry(catalog, schema, info), table_name(info.table) {
}

string OracleIndexEntry::GetSchemaName() const {
	return schema.name;
}

string OracleIndexEntry::GetTableName() const {
	return table_name;
}

} // namespace duckdb
