//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_transaction.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction.hpp"
#include "oracle_connection.hpp"
#include "storage/oracle_connection_pool.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class OracleCatalog;
class OracleSchemaEntry;
class OracleTableEntry;

enum class OracleTransactionState {
	TRANSACTION_NOT_YET_STARTED,
	TRANSACTION_STARTED,
	TRANSACTION_FINISHED
};

class OracleTransaction : public Transaction {
public:
	OracleTransaction(OracleCatalog &oracle_catalog, TransactionManager &manager,
	                   ClientContext &context);
	~OracleTransaction() override;

	void Start();
	void Commit();
	void Rollback();

	OracleConnection &GetConnectionWithoutTransaction();
	OracleConnection &GetConnection();
	optional_ptr<ClientContext> GetContext();

	string GetDSN();
	unique_ptr<OracleResult> Query(const string &query);
	unique_ptr<OracleResult> Query(const string &query,
	                                const unordered_map<string, string> &binds);
	unique_ptr<OracleResult> QueryWithoutTransaction(const string &query);
	void Execute(const string &query);

	static OracleTransaction &Get(ClientContext &context, Catalog &catalog);

	optional_ptr<CatalogEntry> ReferenceEntry(shared_ptr<CatalogEntry> &entry);

	// Rowid registry: populated during UPDATE/DELETE scans.
	// The BIGINT row ID in the scan output is an index into this vector.
	vector<string> rowid_registry;
	void ClearRowidRegistry() { rowid_registry.clear(); }
	int64_t RegisterRowid(const char *ptr, uint32_t len) {
		int64_t idx = (int64_t)rowid_registry.size();
		rowid_registry.emplace_back(ptr, len);
		return idx;
	}
	const string &LookupRowid(int64_t idx) const { return rowid_registry[(idx_t)idx]; }

private:
	OraclePoolConnection connection;
	OracleTransactionState transaction_state;
	AccessMode access_mode;
	reference_map_t<CatalogEntry, shared_ptr<CatalogEntry>> referenced_entries;

private:
	OracleConnection &GetConnectionRaw();
};

} // namespace duckdb
