//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_transaction_manager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"

namespace duckdb {
class OracleCatalog;
class OracleTransaction;

class OracleTransactionManager : public TransactionManager {
public:
	OracleTransactionManager(AttachedDatabase &db_p, OracleCatalog &oracle_catalog);
	// Declared out-of-line so the unique_ptr<OracleTransaction> members are destroyed
	// in a translation unit where OracleTransaction is a complete type.
	~OracleTransactionManager();

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	OracleCatalog &oracle_catalog;
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<OracleTransaction>> transactions;
};

} // namespace duckdb
