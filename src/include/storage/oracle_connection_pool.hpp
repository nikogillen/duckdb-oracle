//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/oracle_connection_pool.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "oracle_connection.hpp"

namespace duckdb {
class OracleCatalog;
class OracleConnectionPool;

class OraclePoolConnection {
public:
	OraclePoolConnection();
	OraclePoolConnection(optional_ptr<OracleConnectionPool> pool, OracleConnection connection);
	~OraclePoolConnection();
	OraclePoolConnection(const OraclePoolConnection &other) = delete;
	OraclePoolConnection &operator=(const OraclePoolConnection &) = delete;
	OraclePoolConnection(OraclePoolConnection &&other) noexcept;
	OraclePoolConnection &operator=(OraclePoolConnection &&) noexcept;

	bool HasConnection();
	OracleConnection &GetConnection();

private:
	optional_ptr<OracleConnectionPool> pool;
	OracleConnection connection;
};

class OracleConnectionPool {
public:
	static constexpr const idx_t DEFAULT_MAX_CONNECTIONS = 64;

	OracleConnectionPool(OracleCatalog &catalog,
	                      idx_t maximum_connections = DEFAULT_MAX_CONNECTIONS);

public:
	bool TryGetConnection(OraclePoolConnection &connection);
	OraclePoolConnection GetConnection();
	OraclePoolConnection ForceGetConnection();
	void ReturnConnection(OracleConnection connection);
	void SetMaximumConnections(idx_t new_max);

	static void OracleSetConnectionCache(ClientContext &context, SetScope scope, Value &parameter);

private:
	OracleCatalog &catalog;
	mutex connection_lock;
	idx_t active_connections;
	idx_t maximum_connections;
	vector<OracleConnection> connection_cache;

private:
	OraclePoolConnection GetConnectionInternal(unique_lock<mutex> &lock);
};

} // namespace duckdb
