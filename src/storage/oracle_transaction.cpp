#include "storage/oracle_transaction.hpp"
#include "storage/oracle_catalog.hpp"
#include "oracle_result.hpp"

namespace duckdb {

OracleTransaction::OracleTransaction(OracleCatalog &oracle_catalog,
                                      TransactionManager &manager,
                                      ClientContext &context)
    : Transaction(manager, context), access_mode(oracle_catalog.access_mode),
      transaction_state(OracleTransactionState::TRANSACTION_NOT_YET_STARTED) {
	connection = oracle_catalog.GetConnectionPool().GetConnection();
}

OracleTransaction::~OracleTransaction() = default;

optional_ptr<ClientContext> OracleTransaction::GetContext() {
	return context.lock();
}

void OracleTransaction::Start() {
	transaction_state = OracleTransactionState::TRANSACTION_NOT_YET_STARTED;
}

void OracleTransaction::Commit() {
	if (transaction_state == OracleTransactionState::TRANSACTION_STARTED) {
		transaction_state = OracleTransactionState::TRANSACTION_FINISHED;
		GetConnectionRaw().Commit();
	}
}

void OracleTransaction::Rollback() {
	if (transaction_state == OracleTransactionState::TRANSACTION_STARTED) {
		transaction_state = OracleTransactionState::TRANSACTION_FINISHED;
		GetConnectionRaw().Rollback();
	}
}

OracleConnection &OracleTransaction::GetConnectionWithoutTransaction() {
	if (transaction_state == OracleTransactionState::TRANSACTION_STARTED) {
		throw std::runtime_error(
		    "Execution without a transaction is not possible if a transaction is already active");
	}
	if (access_mode == AccessMode::READ_ONLY) {
		throw std::runtime_error(
		    "Execution without a transaction is not possible in READ ONLY mode");
	}
	return connection.GetConnection();
}

OracleConnection &OracleTransaction::GetConnection() {
	auto &con = GetConnectionRaw();
	if (transaction_state == OracleTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = OracleTransactionState::TRANSACTION_STARTED;
		// Oracle: no explicit BEGIN needed - transaction starts on first DML.
		// For read-only we can set the session, but that's optional.
		if (access_mode == AccessMode::READ_ONLY) {
			con.Execute(GetContext(), "SET TRANSACTION READ ONLY");
		}
	}
	return con;
}

OracleConnection &OracleTransaction::GetConnectionRaw() {
	return connection.GetConnection();
}

string OracleTransaction::GetDSN() {
	return GetConnectionRaw().GetDSN();
}

unique_ptr<OracleResult> OracleTransaction::Query(const string &query) {
	auto &con = GetConnectionRaw();
	if (transaction_state == OracleTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = OracleTransactionState::TRANSACTION_STARTED;
	}
	return con.Query(GetContext(), query);
}

unique_ptr<OracleResult> OracleTransaction::QueryWithoutTransaction(const string &query) {
	auto &con = GetConnectionRaw();
	if (transaction_state == OracleTransactionState::TRANSACTION_STARTED) {
		throw std::runtime_error(
		    "Execution without a transaction is not possible if a transaction is already active");
	}
	if (access_mode == AccessMode::READ_ONLY) {
		throw std::runtime_error(
		    "Execution without a transaction is not possible in READ ONLY mode");
	}
	return con.Query(GetContext(), query);
}

void OracleTransaction::Execute(const string &query) {
	Query(query);
}

optional_ptr<CatalogEntry> OracleTransaction::ReferenceEntry(shared_ptr<CatalogEntry> &entry) {
	auto &ref = *entry;
	referenced_entries.emplace(ref, entry);
	return ref;
}

OracleTransaction &OracleTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<OracleTransaction>();
}

} // namespace duckdb
