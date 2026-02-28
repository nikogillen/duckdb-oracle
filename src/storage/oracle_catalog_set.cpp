#include "storage/oracle_catalog_set.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_schema_entry.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {

OracleCatalogSet::OracleCatalogSet(OracleCatalog &catalog) : catalog(catalog) {
}

optional_ptr<CatalogEntry> OracleCatalogSet::GetEntry(ClientContext &context,
                                                        OracleTransaction &transaction,
                                                        const string &name) {
	lock_guard<mutex> l(entry_lock);
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context, transaction);
	}
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		if (SupportReload()) {
			auto reloaded = ReloadEntry(transaction, name);
			if (reloaded) {
				return reloaded;
			}
		}
		return nullptr;
	}
	return entry->second.get();
}

void OracleCatalogSet::Scan(ClientContext &context, OracleTransaction &transaction,
                              const std::function<void(CatalogEntry &)> &callback) {
	lock_guard<mutex> l(entry_lock);
	if (!is_loaded) {
		is_loaded = true;
		LoadEntries(context, transaction);
	}
	for (auto &entry : entries) {
		callback(*entry.second);
	}
}

optional_ptr<CatalogEntry> OracleCatalogSet::CreateEntry(OracleTransaction &transaction,
                                                           shared_ptr<CatalogEntry> entry) {
	lock_guard<mutex> l(entry_lock);
	auto &name = entry->name;
	auto result = entry.get();
	entries[name] = std::move(entry);
	return result;
}

void OracleCatalogSet::DropEntry(OracleTransaction &transaction, DropInfo &info) {
	lock_guard<mutex> l(entry_lock);
	auto entry = entries.find(info.name);
	if (entry == entries.end()) {
		if (info.if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw BinderException("Cannot drop entry \"%s\": does not exist", info.name);
		}
		return;
	}
	entries.erase(entry);
}

void OracleCatalogSet::ClearEntries() {
	lock_guard<mutex> l(entry_lock);
	entries.clear();
	is_loaded = false;
}

// ---------------------------------------------------------------------------
// OracleInSchemaSet
// ---------------------------------------------------------------------------

OracleInSchemaSet::OracleInSchemaSet(OracleSchemaEntry &schema, bool loaded)
    : OracleCatalogSet(schema.ParentCatalog().Cast<OracleCatalog>()), schema(schema) {
	if (loaded) {
		is_loaded = true;
	}
}

} // namespace duckdb
