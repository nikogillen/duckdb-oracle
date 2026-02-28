#include "storage/oracle_connection_pool.hpp"
#include "storage/oracle_catalog.hpp"

namespace duckdb {

static bool ora_use_connection_cache = true;

OraclePoolConnection::OraclePoolConnection() : pool(nullptr) {
}

OraclePoolConnection::OraclePoolConnection(optional_ptr<OracleConnectionPool> pool,
                                             OracleConnection connection_p)
    : pool(pool), connection(std::move(connection_p)) {
}

OraclePoolConnection::~OraclePoolConnection() {
	if (pool) {
		pool->ReturnConnection(std::move(connection));
	}
}

OraclePoolConnection::OraclePoolConnection(OraclePoolConnection &&other) noexcept {
	std::swap(pool, other.pool);
	std::swap(connection, other.connection);
}

OraclePoolConnection &OraclePoolConnection::operator=(OraclePoolConnection &&other) noexcept {
	std::swap(pool, other.pool);
	std::swap(connection, other.connection);
	return *this;
}

bool OraclePoolConnection::HasConnection() {
	return pool != nullptr;
}

OracleConnection &OraclePoolConnection::GetConnection() {
	if (!HasConnection()) {
		throw InternalException(
		    "OraclePoolConnection::GetConnection called without a pool");
	}
	return connection;
}

OracleConnectionPool::OracleConnectionPool(OracleCatalog &catalog_p,
                                             idx_t maximum_connections_p)
    : catalog(catalog_p), active_connections(0),
      maximum_connections(maximum_connections_p) {
}

OraclePoolConnection OracleConnectionPool::GetConnectionInternal(unique_lock<mutex> &lock) {
	active_connections++;
	if (!connection_cache.empty()) {
		auto conn = OraclePoolConnection(this, std::move(connection_cache.back()));
		connection_cache.pop_back();
		return conn;
	}
	// Open a new connection (outside the lock)
	lock.unlock();
	return OraclePoolConnection(
	    this,
	    OracleConnection::Open(catalog.connection_string, catalog.attach_path));
}

OraclePoolConnection OracleConnectionPool::ForceGetConnection() {
	unique_lock<mutex> l(connection_lock);
	return GetConnectionInternal(l);
}

bool OracleConnectionPool::TryGetConnection(OraclePoolConnection &connection) {
	unique_lock<mutex> l(connection_lock);
	if (active_connections >= maximum_connections) {
		return false;
	}
	connection = GetConnectionInternal(l);
	return true;
}

OraclePoolConnection OracleConnectionPool::GetConnection() {
	OraclePoolConnection result;
	if (!TryGetConnection(result)) {
		throw IOException(
		    "Failed to get connection from OracleConnectionPool - maximum connection count exceeded (%llu/%llu max)",
		    active_connections, maximum_connections);
	}
	return result;
}

void OracleConnectionPool::ReturnConnection(OracleConnection connection) {
	unique_lock<mutex> l(connection_lock);
	if (active_connections <= 0) {
		throw InternalException(
		    "OracleConnectionPool::ReturnConnection called but active_connections is 0");
	}
	if (!ora_use_connection_cache) {
		active_connections--;
		return;
	}
	// Check if connection is still healthy
	l.unlock();
	bool connection_is_bad = !connection.IsConnectionOk();
	l.lock();
	active_connections--;
	if (connection_is_bad) {
		return;
	}
	if (active_connections >= maximum_connections) {
		return;
	}
	connection_cache.push_back(std::move(connection));
}

void OracleConnectionPool::SetMaximumConnections(idx_t new_max) {
	lock_guard<mutex> l(connection_lock);
	if (new_max < maximum_connections) {
		auto total_open = active_connections + connection_cache.size();
		while (!connection_cache.empty() && total_open > new_max) {
			total_open--;
			connection_cache.pop_back();
		}
	}
	maximum_connections = new_max;
}

void OracleConnectionPool::OracleSetConnectionCache(ClientContext &context, SetScope scope,
                                                      Value &parameter) {
	if (parameter.IsNull()) {
		throw BinderException("Cannot be set to NULL");
	}
	ora_use_connection_cache = BooleanValue::Get(parameter);
}

} // namespace duckdb
