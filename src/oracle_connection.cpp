#include "oracle_connection.hpp"
#include "oracle_logging.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/helper.hpp"

#include <chrono>

namespace duckdb {

static bool debug_oracle_print_queries = false;

// ---------------------------------------------------------------------------
// OwnedOracleConnection
// ---------------------------------------------------------------------------

OwnedOracleConnection::OwnedOracleConnection(dpiConn *conn) : connection(conn) {
}

OwnedOracleConnection::~OwnedOracleConnection() {
	if (!connection) {
		return;
	}
	dpiConn_release(connection);
	connection = nullptr;
}

// ---------------------------------------------------------------------------
// OracleConnection
// ---------------------------------------------------------------------------

OracleConnection::OracleConnection(shared_ptr<OwnedOracleConnection> connection_p)
    : connection(std::move(connection_p)) {
}

OracleConnection::~OracleConnection() {
	Close();
}

OracleConnection::OracleConnection(OracleConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
}

OracleConnection &OracleConnection::operator=(OracleConnection &&other) noexcept {
	std::swap(connection, other.connection);
	std::swap(dsn, other.dsn);
	return *this;
}

OracleConnection OracleConnection::Open(const string &dsn, const string &attach_path) {
	OracleConnection result;
	result.connection =
	    make_shared_ptr<OwnedOracleConnection>(OracleUtils::OraConnect(dsn, attach_path));
	result.dsn = dsn;
	return result;
}

// ---------------------------------------------------------------------------
// Internal query execution: materialise all rows into OracleResult
// ---------------------------------------------------------------------------

unique_ptr<OracleResult> OracleConnection::ExecuteQuery(const string &query,
                                                         const unordered_map<string, string> &binds) {
	auto conn = GetConn();
	dpiStmt *stmt = nullptr;
	dpiErrorInfo error_info;

	auto result = make_uniq<OracleResult>();

	if (dpiConn_prepareStmt(conn, 0, query.c_str(), (uint32_t)query.size(),
	                         nullptr, 0, &stmt) < 0) {
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		throw IOException("Oracle prepare failed for query \"%s\": %s", query,
		                  string(error_info.message));
	}

	// Bind named parameters (e.g. :owner, :table_name) by name.
	// Keys in |binds| must match the placeholder name without the leading colon.
	for (const auto &kv : binds) {
		const string &name  = kv.first;
		const string &value = kv.second;
		dpiData bind_data;
		// dpiData_setBytes sets isNull=0 and the asBytes fields.
		dpiData_setBytes(&bind_data, const_cast<char *>(value.c_str()),
		                 (uint32_t)value.size());
		if (dpiStmt_bindValueByName(stmt, name.c_str(), (uint32_t)name.size(),
		                             DPI_NATIVE_TYPE_BYTES, &bind_data) < 0) {
			dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
			dpiStmt_release(stmt);
			throw IOException("Oracle bind by name failed for ':%s': %s", name,
			                  string(error_info.message));
		}
	}

	uint32_t num_cols = 0;
	if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &num_cols) < 0) {
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		dpiStmt_release(stmt);
		throw IOException("Oracle execute failed for query \"%s\": %s", query,
		                  string(error_info.message));
	}

	if (num_cols == 0) {
		// DML statement
		uint64_t row_count = 0;
		dpiStmt_getRowCount(stmt, &row_count);
		result->affected_rows = row_count;
		dpiStmt_release(stmt);
		return result;
	}

	// SELECT: fetch all rows
	int found = 0;
	uint32_t buffer_row_index = 0;
	while (true) {
		if (dpiStmt_fetch(stmt, &found, &buffer_row_index) < 0) {
			dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
			dpiStmt_release(stmt);
			throw IOException("Oracle fetch failed for query \"%s\": %s", query,
			                  string(error_info.message));
		}
		if (!found) {
			break;
		}
		vector<OracleResultCell> row(num_cols);
		for (uint32_t c = 1; c <= num_cols; c++) {
			dpiNativeTypeNum native_type;
			dpiData *data;
			if (dpiStmt_getQueryValue(stmt, c, &native_type, &data) < 0) {
				dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
				dpiStmt_release(stmt);
				throw IOException("Oracle getQueryValue failed: %s", string(error_info.message));
			}
			auto &cell = row[c - 1];
			if (data->isNull) {
				cell.is_null = true;
				continue;
			}
			cell.is_null = false;
			// Convert to string representation for the result
			switch (native_type) {
			case DPI_NATIVE_TYPE_INT64:
				cell.value = to_string(data->value.asInt64);
				break;
			case DPI_NATIVE_TYPE_UINT64:
				cell.value = to_string(data->value.asUint64);
				break;
			case DPI_NATIVE_TYPE_FLOAT:
				cell.value = to_string(data->value.asFloat);
				break;
			case DPI_NATIVE_TYPE_DOUBLE:
				cell.value = to_string(data->value.asDouble);
				break;
			case DPI_NATIVE_TYPE_BYTES:
				cell.value = string(data->value.asBytes.ptr, data->value.asBytes.length);
				break;
			case DPI_NATIVE_TYPE_TIMESTAMP: {
				auto &ts = data->value.asTimestamp;
				char buf[64];
				snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
				         ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.second,
				         ts.fsecond / 1000);
				cell.value = buf;
				break;
			}
			case DPI_NATIVE_TYPE_INTERVAL_DS: {
				auto &ivl = data->value.asIntervalDS;
				char buf[64];
				snprintf(buf, sizeof(buf), "%d %02d:%02d:%02d.%06d", ivl.days,
				         ivl.hours, ivl.minutes, ivl.seconds, ivl.fseconds / 1000);
				cell.value = buf;
				break;
			}
			case DPI_NATIVE_TYPE_INTERVAL_YM: {
				auto &ivl = data->value.asIntervalYM;
				char buf[32];
				snprintf(buf, sizeof(buf), "%d-%d", ivl.years, ivl.months);
				cell.value = buf;
				break;
			}
			case DPI_NATIVE_TYPE_BOOLEAN:
				cell.value = data->value.asBoolean ? "1" : "0";
				break;
			default:
				cell.value = "";
				cell.is_null = true;
				break;
			}
		}
		result->rows.push_back(std::move(row));
	}

	dpiStmt_release(stmt);
	return result;
}

// ---------------------------------------------------------------------------
// Public query API
// ---------------------------------------------------------------------------

unique_ptr<OracleResult> OracleConnection::TryQuery(optional_ptr<ClientContext> context,
                                                     const string &query,
                                                     optional_ptr<string> error_message) {
	lock_guard<mutex> guard(connection->connection_lock);
	if (DebugPrintQueries()) {
		Printer::Print(query + "\n");
	}
	try {
		return ExecuteQuery(query);
	} catch (std::exception &ex) {
		if (error_message) {
			*error_message = ex.what();
		}
		return nullptr;
	}
}

unique_ptr<OracleResult> OracleConnection::Query(optional_ptr<ClientContext> context,
                                                   const string &query) {
	string error_msg;
	auto result = TryQuery(context, query, &error_msg);
	if (!result) {
		throw std::runtime_error(error_msg);
	}
	return result;
}

void OracleConnection::Execute(optional_ptr<ClientContext> context, const string &query) {
	Query(context, query);
}

// ---------------------------------------------------------------------------
// Bind-parameter overloads (named parameters via dpiStmt_bindValueByName)
// ---------------------------------------------------------------------------

unique_ptr<OracleResult> OracleConnection::TryQuery(optional_ptr<ClientContext> context,
                                                     const string &query,
                                                     const unordered_map<string, string> &binds,
                                                     optional_ptr<string> error_message) {
	lock_guard<mutex> guard(connection->connection_lock);
	if (DebugPrintQueries()) {
		string debug_line = query;
		if (!binds.empty()) {
			debug_line += "  -- binds:";
			for (const auto &kv : binds) {
				debug_line += " :" + kv.first + "='" + kv.second + "'";
			}
		}
		Printer::Print(debug_line + "\n");
	}
	try {
		return ExecuteQuery(query, binds);
	} catch (std::exception &ex) {
		if (error_message) {
			*error_message = ex.what();
		}
		return nullptr;
	}
}

unique_ptr<OracleResult> OracleConnection::Query(optional_ptr<ClientContext> context,
                                                   const string &query,
                                                   const unordered_map<string, string> &binds) {
	string error_msg;
	auto result = TryQuery(context, query, binds, &error_msg);
	if (!result) {
		throw std::runtime_error(error_msg);
	}
	return result;
}

void OracleConnection::Commit() {
	if (!connection || !connection->connection) {
		return;
	}
	dpiErrorInfo error_info;
	if (dpiConn_commit(connection->connection) < 0) {
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		throw IOException("Oracle COMMIT failed: %s", string(error_info.message));
	}
}

void OracleConnection::Rollback() {
	if (!connection || !connection->connection) {
		return;
	}
	dpiConn_rollback(connection->connection); // best-effort
}

OracleVersion OracleConnection::GetOracleVersion() {
	return OracleUtils::GetVersion(GetConn());
}

bool OracleConnection::IsConnectionOk() {
	if (!connection || !connection->connection) {
		return false;
	}
	// Client-side health check: no server round-trip, unlike the previous
	// "SELECT 1 FROM DUAL". Called on every pool return, so this saves a full
	// network round-trip per pooled query.
	int is_healthy = 0;
	if (dpiConn_getIsHealthy(connection->connection, &is_healthy) < 0) {
		return false;
	}
	return is_healthy != 0;
}

bool OracleConnection::IsOpen() {
	return connection.get() != nullptr;
}

void OracleConnection::Close() {
	if (!IsOpen()) {
		return;
	}
	connection = nullptr;
}

void OracleConnection::DebugSetPrintQueries(bool print) {
	debug_oracle_print_queries = print;
}

bool OracleConnection::DebugPrintQueries() {
	return debug_oracle_print_queries;
}

} // namespace duckdb
