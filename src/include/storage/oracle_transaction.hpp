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
	unique_ptr<OracleResult> QueryWithoutTransaction(const string &query);
	void Execute(const string &query);

	static OracleTransaction &Get(ClientContext &context, Catalog &catalog);

	optional_ptr<CatalogEntry> ReferenceEntry(shared_ptr<CatalogEntry> &entry);

private:
	OraclePoolConnection connection;
	OracleTransactionState transaction_state;
	AccessMode access_mode;
	reference_map_t<CatalogEntry, shared_ptr<CatalogEntry>> referenced_entries;

private:
	OracleConnection &GetConnectionRaw();
};

} // namespace duckdb
