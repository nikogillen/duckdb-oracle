//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_catalog_set.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/mutex.hpp"
#include "oracle_result.hpp"

namespace duckdb {
class OracleCatalog;
class OracleSchemaEntry;
class OracleTransaction;

struct OracleResultSlice {
	OracleResult result;
	idx_t start;
	idx_t end;

	OracleResult &GetResult() {
		return result;
	}
};

class OracleCatalogSet {
public:
	explicit OracleCatalogSet(OracleCatalog &catalog);
	virtual ~OracleCatalogSet() = default;

public:
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, OracleTransaction &transaction,
	                                    const string &name);
	void Scan(ClientContext &context, OracleTransaction &transaction,
	          const std::function<void(CatalogEntry &)> &callback);
	optional_ptr<CatalogEntry> CreateEntry(OracleTransaction &transaction,
	                                        shared_ptr<CatalogEntry> entry);
	void DropEntry(OracleTransaction &transaction, DropInfo &info);
	void ClearEntries();

protected:
	// Called from within LoadEntries/ReloadEntry while entry_lock is already held.
	optional_ptr<CatalogEntry> CreateEntryInternal(shared_ptr<CatalogEntry> entry);

	virtual void LoadEntries(ClientContext &context, OracleTransaction &transaction) = 0;
	virtual bool SupportReload() const {
		return false;
	}
	virtual optional_ptr<CatalogEntry> ReloadEntry(OracleTransaction &transaction,
	                                                const string &name) {
		return nullptr;
	}

protected:
	OracleCatalog &catalog;
	mutex entry_lock;
	case_insensitive_map_t<shared_ptr<CatalogEntry>> entries;
	bool is_loaded = false;
};

class OracleInSchemaSet : public OracleCatalogSet {
public:
	OracleInSchemaSet(OracleSchemaEntry &schema, bool loaded = false);

protected:
	OracleSchemaEntry &schema;
};

} // namespace duckdb
