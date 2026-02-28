#include "storage/oracle_type_entry.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"

namespace duckdb {

OracleTypeEntry::OracleTypeEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                  CreateTypeInfo &info)
    : TypeCatalogEntry(catalog, schema, info) {
}

} // namespace duckdb
