//===----------------------------------------------------------------------===//
//
// storage/oracle_view_entry.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"

namespace duckdb {

//! Minimal catalog entry for Oracle views — used only so duckdb_views() can
//! safely cast VIEW_ENTRY entries to ViewCatalogEntry and display them as VIEW.
//!
//! Oracle views are SCANNED via OracleTableEntry/GetScanFunction.  This class
//! only exists for catalog metadata display; it is never used for execution.
class OracleViewEntry : public ViewCatalogEntry {
public:
	OracleViewEntry(Catalog &catalog, SchemaCatalogEntry &schema, const string &view_name);

private:
	//! Delegating constructor: receives an already-constructed CreateViewInfo by value
	//! so the base class gets a non-const lvalue reference (its required signature).
	OracleViewEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateViewInfo info_storage);
	//! Build a minimal CreateViewInfo (no SQL, no columns — Oracle views are opaque).
	static CreateViewInfo MakeViewInfo(const string &view_name);
};

} // namespace duckdb
