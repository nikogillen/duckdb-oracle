#include "oracle_utils.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <cctype>
#include <cstring>
#ifdef _WIN32
#include "duckdb/common/windows.hpp"
#endif

namespace duckdb {

dpiContext *OracleUtils::g_dpi_context = nullptr;
mutex OracleUtils::g_context_mutex;

dpiContext *OracleUtils::GetOrCreateContext() {
	lock_guard<mutex> l(g_context_mutex);
	if (g_dpi_context) {
		return g_dpi_context;
	}
	dpiErrorInfo error_info;
	dpiContextCreateParams params;
	memset(&params, 0, sizeof(params));
	if (dpiContext_createWithParams(DPI_MAJOR_VERSION, DPI_MINOR_VERSION, &params,
	                                &g_dpi_context, &error_info) < 0) {
		throw IOException("Failed to create ODPI-C context: %.*s (fn=%s)",
		                  (int)error_info.messageLength, error_info.message,
		                  error_info.fnName);
	}
	return g_dpi_context;
}

// Build a display string that never contains the password, for use in error messages.
static string RedactTarget(const string &user, const string &connect_string) {
	if (!connect_string.empty()) {
		return user.empty() ? connect_string : user + "@" + connect_string;
	}
	return user.empty() ? "<oracle>" : user;
}

void OracleUtils::ParseDSN(const string &dsn, string &user, string &password,
                            string &connect_string) {
	// Try key-value format first: "user=xxx password=yyy connectString=//..."
	if (dsn.find('=') != string::npos && dsn.find('/') == string::npos) {
		// Simple key=value parser
		auto parts = StringUtil::Split(dsn, ' ');
		for (auto &part : parts) {
			auto eq = part.find('=');
			if (eq == string::npos) {
				continue;
			}
			auto key = StringUtil::Lower(part.substr(0, eq));
			auto val = part.substr(eq + 1);
			if (key == "user" || key == "username") {
				user = val;
			} else if (key == "password" || key == "pass") {
				password = val;
			} else if (key == "connectstring" || key == "dsn" || key == "host") {
				connect_string = val;
			}
		}
		return;
	}

	// Oracle format: "user/password@connect_string"
	// OR: "user/password" (no connect string - uses ORACLE_SID/TWO_TASK env)
	auto at_pos = dsn.rfind('@');
	if (at_pos != string::npos) {
		connect_string = dsn.substr(at_pos + 1);
		auto user_pass = dsn.substr(0, at_pos);
		auto slash_pos = user_pass.find('/');
		if (slash_pos != string::npos) {
			user = user_pass.substr(0, slash_pos);
			password = user_pass.substr(slash_pos + 1);
		} else {
			user = user_pass;
		}
	} else {
		// No @ sign - entire string might be just user/pass or user
		auto slash_pos = dsn.find('/');
		if (slash_pos != string::npos) {
			user = dsn.substr(0, slash_pos);
			password = dsn.substr(slash_pos + 1);
		} else {
			// Use as connect string, rely on external auth or wallets
			connect_string = dsn;
		}
	}
}

// Serializes the TNS_ADMIN environment manipulation below. Oracle resolves
// tnsnames.ora/sqlnet.ora/wallets from TNS_ADMIN at connect time, and the variable
// is process-wide — so connects that carry a config directory must not overlap.
static mutex g_tns_admin_mutex;

// RAII guard that points TNS_ADMIN at `config_dir` for the lifetime of a connect
// and restores the previous value (or unsets it) afterwards. An empty config_dir
// leaves the environment untouched.
class ScopedTnsAdmin {
public:
	explicit ScopedTnsAdmin(const string &config_dir) {
		if (config_dir.empty()) {
			return;
		}
#ifdef _WIN32
		// Read and write through the same (Win32) environment: it is not kept in
		// sync with the CRT environment that getenv() sees.
		char buffer[MAX_PATH];
		auto length = GetEnvironmentVariableA("TNS_ADMIN", buffer, sizeof(buffer));
		had_previous = length > 0 && length < sizeof(buffer);
		if (had_previous) {
			previous_value.assign(buffer, length);
		}
		changed = SetEnvironmentVariableA("TNS_ADMIN", config_dir.c_str()) != 0;
#else
		const char *previous = getenv("TNS_ADMIN");
		had_previous = previous != nullptr;
		if (had_previous) {
			previous_value = previous;
		}
		changed = setenv("TNS_ADMIN", config_dir.c_str(), 1) == 0;
#endif
	}

	~ScopedTnsAdmin() {
		if (!changed) {
			return;
		}
#ifdef _WIN32
		SetEnvironmentVariableA("TNS_ADMIN", had_previous ? previous_value.c_str() : nullptr);
#else
		if (had_previous) {
			setenv("TNS_ADMIN", previous_value.c_str(), 1);
		} else {
			unsetenv("TNS_ADMIN");
		}
#endif
	}

private:
	bool changed = false;
	bool had_previous = false;
	string previous_value;
};

dpiConn *OracleUtils::OraConnect(const string &dsn, const string &attach_path,
                                  const string &config_dir) {
	// Take the context lock first (inside GetOrCreateContext) and only then the
	// environment lock, so the two are never acquired in the opposite order.
	auto context = GetOrCreateContext();

	string user, password, connect_string;
	ParseDSN(dsn, user, password, connect_string);

	// Hold the lock across the connect even without a config directory: TNS_ADMIN is
	// process-wide, so a plain connect must not observe another connect's setting.
	// This serializes connects; acceptable because the pool caches connections, so
	// connects are rare. (A shared_mutex would let plain connects run in parallel,
	// but the extension targets C++11.)
	lock_guard<mutex> env_lock(g_tns_admin_mutex);
	ScopedTnsAdmin tns_admin(config_dir);

	dpiConn *conn = nullptr;
	dpiErrorInfo error_info;

	// Enable OCI threaded mode: the extension shares connections across DuckDB scan
	// threads via the connection pool, which is unsafe without DPI_MODE_CREATE_THREADED.
	dpiCommonCreateParams common_params;
	if (dpiContext_initCommonCreateParams(context, &common_params) < 0) {
		dpiContext_getError(context, &error_info);
		throw IOException("Failed to init Oracle common params: %.*s",
		                  (int)error_info.messageLength, error_info.message);
	}
	common_params.createMode |= DPI_MODE_CREATE_THREADED;

	dpiConnCreateParams conn_params;
	dpiContext_initConnCreateParams(context, &conn_params);

	// External authentication (Oracle Wallet holding the credentials, "/@alias"):
	// no user and no password, but a connect string. ODPI-C requires the flag to be
	// set explicitly and rejects it when credentials are also supplied.
	if (user.empty() && password.empty() && !connect_string.empty()) {
		conn_params.externalAuth = 1;
	}

	if (dpiConn_create(context, user.c_str(), (uint32_t)user.size(), password.c_str(),
	                   (uint32_t)password.size(), connect_string.c_str(),
	                   (uint32_t)connect_string.size(), &common_params, &conn_params,
	                   &conn) < 0) {
		dpiContext_getError(context, &error_info);
		// Never expose the password in error messages: build a redacted target string
		// from the parsed components (attach_path may itself contain the full DSN).
		string safe_target = RedactTarget(user, connect_string);
		throw IOException("Unable to connect to Oracle at \"%s\": %.*s", safe_target,
		                  (int)error_info.messageLength, error_info.message);
	}

	// Best-effort session tagging so DBAs can identify the extension in V$SESSION
	// (CLIENT_IDENTIFIER / MODULE). Failures here are non-fatal.
	static const char kClientId[] = "duckdb_oracle";
	static const char kModule[] = "duckdb-oracle";
	dpiConn_setClientIdentifier(conn, kClientId, (uint32_t)(sizeof(kClientId) - 1));
	dpiConn_setModule(conn, kModule, (uint32_t)(sizeof(kModule) - 1));
	return conn;
}

OracleVersion OracleUtils::GetVersion(dpiConn *conn) {
	OracleVersion version;
	dpiVersionInfo ver_info;
	if (dpiConn_getServerVersion(conn, nullptr, nullptr, &ver_info) < 0) {
		return version;
	}
	version.major_v = ver_info.versionNum;
	version.minor_v = ver_info.releaseNum;
	version.patch_v = ver_info.updateNum;
	version.port_v = ver_info.portReleaseNum;
	version.port_patch_v = ver_info.portUpdateNum;
	return version;
}

LogicalType OracleUtils::TypeToLogicalType(const OracleTypeData &type_info,
                                            OracleType &oracle_type) {
	auto type_name = StringUtil::Upper(type_info.type_name);

	// Strip inline precision from type name while preserving suffixes.
	// e.g. "TIMESTAMP(6)"                   -> "TIMESTAMP"
	//      "TIMESTAMP(6) WITH TIME ZONE"     -> "TIMESTAMP WITH TIME ZONE"
	//      "TIMESTAMP(6) WITH LOCAL TIME ZONE" -> "TIMESTAMP WITH LOCAL TIME ZONE"
	string base_type;
	auto paren_pos = type_name.find('(');
	if (paren_pos != string::npos) {
		auto close_pos = type_name.find(')', paren_pos);
		if (close_pos != string::npos && close_pos + 1 < type_name.size()) {
			// Rejoin: everything before '(' + everything after ')'
			base_type = type_name.substr(0, paren_pos) + type_name.substr(close_pos + 1);
		} else {
			base_type = type_name.substr(0, paren_pos);
		}
	} else {
		base_type = type_name;
	}
	StringUtil::Trim(base_type);

	if (base_type == "NUMBER" || base_type == "NUMERIC" || base_type == "DECIMAL") {
		// NUMBER with precision and scale
		if (type_info.data_precision > 0 && type_info.data_scale >= 0) {
			if (type_info.data_scale == 0 && type_info.data_precision <= 18) {
				// Integer-like number
				if (type_info.data_precision <= 4) {
					return LogicalType::SMALLINT;
				} else if (type_info.data_precision <= 9) {
					return LogicalType::INTEGER;
				} else {
					return LogicalType::BIGINT;
				}
			}
			if (type_info.data_precision <= 38 && type_info.data_scale >= 0) {
				return LogicalType::DECIMAL((uint8_t)type_info.data_precision,
				                            (uint8_t)type_info.data_scale);
			}
		}
		// Unconstrained NUMBER -> DOUBLE
		oracle_type.info = OracleTypeAnnotation::NUMBER_AS_DOUBLE;
		return LogicalType::DOUBLE;
	} else if (base_type == "FLOAT") {
		// Oracle FLOAT is NUMBER with a binary precision - map to DOUBLE
		return LogicalType::DOUBLE;
	} else if (base_type == "BINARY_FLOAT") {
		return LogicalType::FLOAT;
	} else if (base_type == "BINARY_DOUBLE") {
		return LogicalType::DOUBLE;
	} else if (base_type == "INTEGER" || base_type == "INT" || base_type == "SMALLINT") {
		return LogicalType::BIGINT;
	} else if (base_type == "VARCHAR2" || base_type == "NVARCHAR2" || base_type == "VARCHAR") {
		return LogicalType::VARCHAR;
	} else if (base_type == "CHAR" || base_type == "NCHAR") {
		return LogicalType::VARCHAR;
	} else if (base_type == "CLOB" || base_type == "NCLOB" || base_type == "LONG") {
		oracle_type.info = OracleTypeAnnotation::CLOB_AS_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "BLOB") {
		return LogicalType::BLOB;
	} else if (base_type == "RAW" || base_type == "LONG RAW") {
		return LogicalType::BLOB;
	} else if (base_type == "DATE") {
		// Oracle DATE includes time: map to TIMESTAMP
		oracle_type.info = OracleTypeAnnotation::DATE_AS_TIMESTAMP;
		return LogicalType::TIMESTAMP;
	} else if (base_type == "TIMESTAMP") {
		return LogicalType::TIMESTAMP;
	} else if (base_type == "TIMESTAMP WITH TIME ZONE") {
		oracle_type.info = OracleTypeAnnotation::TIMESTAMP_WITH_TZ;
		return LogicalType::TIMESTAMP_TZ;
	} else if (base_type == "TIMESTAMP WITH LOCAL TIME ZONE") {
		oracle_type.info = OracleTypeAnnotation::TIMESTAMP_WITH_LTZ;
		return LogicalType::TIMESTAMP;
	} else if (base_type == "INTERVAL DAY TO SECOND" ||
	           StringUtil::StartsWith(base_type, "INTERVAL DAY")) {
		return LogicalType::INTERVAL;
	} else if (base_type == "INTERVAL YEAR TO MONTH" ||
	           StringUtil::StartsWith(base_type, "INTERVAL YEAR")) {
		// Map year-month intervals to VARCHAR (DuckDB INTERVAL is day-time based)
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "XMLTYPE") {
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "ROWID" || base_type == "UROWID") {
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "SDO_GEOMETRY" || base_type == "MDSYS.SDO_GEOMETRY") {
		// Oracle Spatial: the scan selects SDO_UTIL.TO_WKTGEOMETRY(col), so the value
		// arrives as WKT text. On DuckDB 1.5+ we surface it as a native GEOMETRY
		// (with the column's SRID as CRS when known); on 1.4 LTS, which has no
		// GEOMETRY type, the WKT stays a VARCHAR.
		oracle_type.info = OracleTypeAnnotation::SPATIAL_AS_GEOMETRY;
#if ORACLE_HAS_GEOMETRY_TYPE
		if (type_info.srid > 0) {
			return LogicalType::GEOMETRY("EPSG:" + to_string(type_info.srid));
		}
		return LogicalType::GEOMETRY();
#else
		return LogicalType::VARCHAR;
#endif
	} else if (base_type == "BOOLEAN") {
		// Oracle 23c+ BOOLEAN; older Oracle lacks this
		return LogicalType::BOOLEAN;
	} else if (base_type == "JSON") {
		// Oracle 21c+ native JSON → DuckDB JSON
		oracle_type.info = OracleTypeAnnotation::JSON_AS_JSON;
		return LogicalType::JSON();
	} else if (base_type == "VECTOR" || StringUtil::StartsWith(base_type, "VECTOR")) {
		// Oracle 23ai VECTOR → LIST(FLOAT); fetched via dpiVector
		oracle_type.info = OracleTypeAnnotation::VECTOR_AS_LIST;
		return LogicalType::LIST(LogicalType::FLOAT);
	} else {
		// Unknown/unsupported type - cast to VARCHAR
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	}
}

string OracleUtils::TypeToString(const LogicalType &input) {
	switch (input.id()) {
	case LogicalTypeId::BOOLEAN:
		return "NUMBER(1)"; // Oracle 12c/18c: use NUMBER(1) for boolean
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
		return "NUMBER(5)";
	case LogicalTypeId::INTEGER:
		return "NUMBER(10)";
	case LogicalTypeId::BIGINT:
		return "NUMBER(19)";
	case LogicalTypeId::UTINYINT:
		return "NUMBER(3)";
	case LogicalTypeId::USMALLINT:
		return "NUMBER(5)";
	case LogicalTypeId::UINTEGER:
		return "NUMBER(10)";
	case LogicalTypeId::UBIGINT:
		return "NUMBER(20)";
	case LogicalTypeId::FLOAT:
		return "BINARY_FLOAT";
	case LogicalTypeId::DOUBLE:
		return "BINARY_DOUBLE";
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(input);
		auto scale = DecimalType::GetScale(input);
		return StringUtil::Format("NUMBER(%d,%d)", width, scale);
	}
	case LogicalTypeId::VARCHAR:
		return "CLOB";
	case LogicalTypeId::BLOB:
		return "BLOB";
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return "TIMESTAMP(6)";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "TIMESTAMP(6) WITH TIME ZONE";
	case LogicalTypeId::TIME:
		return "VARCHAR2(15)"; // Oracle has no TIME type
	case LogicalTypeId::TIME_TZ:
		return "VARCHAR2(21)";
	case LogicalTypeId::INTERVAL:
		return "INTERVAL DAY(9) TO SECOND(6)";
	case LogicalTypeId::HUGEINT:
		return "NUMBER(39)";
	case LogicalTypeId::UUID:
		return "VARCHAR2(36)";
	default:
		return "CLOB";
	}
}

LogicalType OracleUtils::ToOracleType(const LogicalType &input) {
	switch (input.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::UUID:
		return input;
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return LogicalType::TIMESTAMP;
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
		return LogicalType::VARCHAR;
	case LogicalTypeId::HUGEINT:
		return LogicalType::DOUBLE;
	default:
		return LogicalType::VARCHAR;
	}
}

string OracleUtils::QuoteIdentifier(const string &text) {
	// The catalog stores every identifier folded to lower case (the load queries use
	// LOWER(owner/table_name/column_name)). Historically the generated SQL left names
	// unquoted and relied on Oracle up-casing unquoted identifiers to match the actual
	// (upper-case) dictionary entries — but that left the door open to SQL injection
	// through maliciously named objects.
	//
	// We reproduce Oracle's unquoted-identifier normalization by upper-casing the name,
	// then wrap it in double quotes and escape any embedded double quote by doubling it.
	// This targets exactly the same objects as before (standard upper-case Oracle
	// schemas/tables/columns) while making injection impossible: a crafted name can only
	// ever become a single quoted identifier, never break out into surrounding SQL.
	//
	// Known limitation (pre-existing): case-sensitive objects created with quoted
	// lower/mixed-case names are not addressable — matching the extension's prior
	// behaviour. Preserving original dictionary case end-to-end is tracked separately.
	auto upper = StringUtil::Upper(text);
	string result = "\"";
	for (char c : upper) {
		if (c == '"') {
			result += "\"\"";
		} else {
			result += c;
		}
	}
	result += "\"";
	return result;
}

string OracleUtils::WriteLiteral(const string &value) {
	string result = "'";
	for (char c : value) {
		if (c == '\'') {
			result += "''";
		} else {
			result += c;
		}
	}
	result += "'";
	return result;
}

string OracleUtils::ValueToOracleSQL(const Value &val) {
	if (val.IsNull()) {
		return "NULL";
	}
	switch (val.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return val.GetValue<bool>() ? "1" : "0";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::HUGEINT:
		return val.ToString();
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		return WriteLiteral(val.ToString());
	case LogicalTypeId::DATE: {
		auto d = DateValue::Get(val);
		int32_t y, mo, day;
		Date::Convert(d, y, mo, day);
		return StringUtil::Format("DATE '%04d-%02d-%02d'", y, mo, day);
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS: {
		auto ts = TimestampValue::Get(val);
		auto d = Timestamp::GetDate(ts);
		auto t = Timestamp::GetTime(ts);
		int32_t y, mo, day, h, m, s, micros;
		Date::Convert(d, y, mo, day);
		Time::Convert(t, h, m, s, micros);
		return StringUtil::Format("TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d.%06d'",
		                          y, mo, day, h, m, s, micros);
	}
	default:
		return WriteLiteral(val.ToString());
	}
}

} // namespace duckdb
