//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_scanner.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "oracle_utils.hpp"
#include "oracle_connection.hpp"
#include "storage/oracle_connection_pool.hpp"

namespace duckdb {
class OracleCatalog;
struct OracleLocalState;
struct OracleGlobalState;
class OracleTransaction;

struct OracleBindData : public FunctionData {
	// Default rows to fetch per ODPI-C array fetch call
	static constexpr const idx_t DEFAULT_FETCH_ROWS = 2000;

public:
	OracleBindData();

	OracleVersion version;
	string schema_name;
	string table_name;
	string sql;
	string limit;

	vector<OracleType> oracle_types;
	vector<string> names;
	vector<string> oracle_names; // column names from Oracle data dictionary
	vector<LogicalType> types;

	idx_t fetch_rows = DEFAULT_FETCH_ROWS;
	idx_t approx_num_rows = 0;
	string dsn;
	string attach_path;

	bool requires_materialization = true;
	bool can_use_main_thread = true;
	bool read_only = true;
	bool use_transaction = true;
	idx_t max_threads = 1;

public:
	void SetCatalog(OracleCatalog &catalog);
	void SetTable(OracleTableEntry &table);
	optional_ptr<OracleCatalog> GetCatalog() const {
		return ora_catalog;
	}
	optional_ptr<OracleTableEntry> GetTable() const {
		return ora_table;
	}

	unique_ptr<FunctionData> Copy() const override {
		throw NotImplementedException("OracleBindData::Copy");
	}
	bool Equals(const FunctionData &other_p) const override {
		return false;
	}

private:
	optional_ptr<OracleCatalog> ora_catalog;
	optional_ptr<OracleTableEntry> ora_table;
};

class OracleAttachFunction : public TableFunction {
public:
	OracleAttachFunction();
};

class OracleScanFunction : public TableFunction {
public:
	OracleScanFunction();

	static void PrepareBind(OracleVersion version, ClientContext &context,
	                        OracleBindData &bind, idx_t approx_num_rows);
};

class OracleScanFunctionFilterPushdown : public TableFunction {
public:
	OracleScanFunctionFilterPushdown();
};

class OracleClearCacheFunction : public TableFunction {
public:
	OracleClearCacheFunction();

	static void ClearCacheOnSetting(ClientContext &context, SetScope scope, Value &parameter);
	static void ClearOracleCaches(ClientContext &context);
};

class OracleQueryFunction : public TableFunction {
public:
	OracleQueryFunction();
};

class OracleExecuteFunction : public TableFunction {
public:
	OracleExecuteFunction();
};

} // namespace duckdb
