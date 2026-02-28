#include "oracle_utils.hpp"

#include <cctype>
#include <cstring>

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
		throw IOException("Failed to create ODPI-C context: %s (fn=%s)",
		                  error_info.message, error_info.fnName);
	}
	return g_dpi_context;
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

dpiConn *OracleUtils::OraConnect(const string &dsn, const string &attach_path) {
	auto context = GetOrCreateContext();

	string user, password, connect_string;
	ParseDSN(dsn, user, password, connect_string);

	dpiConn *conn = nullptr;
	dpiErrorInfo error_info;

	dpiConnCreateParams conn_params;
	dpiContext_initConnCreateParams(context, &conn_params);

	if (dpiConn_create(context, user.c_str(), (uint32_t)user.size(), password.c_str(),
	                   (uint32_t)password.size(), connect_string.c_str(),
	                   (uint32_t)connect_string.size(), nullptr, &conn_params,
	                   &conn) < 0) {
		dpiContext_getError(context, &error_info);
		throw IOException("Unable to connect to Oracle at \"%s\": %s", attach_path,
		                  string(error_info.message));
	}
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

	// Strip trailing precision info from type name, e.g. "TIMESTAMP(6)" -> "TIMESTAMP"
	auto paren_pos = type_name.find('(');
	string base_type = (paren_pos != string::npos) ? type_name.substr(0, paren_pos) : type_name;
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
		// Oracle Spatial geometry - cast to VARCHAR
		oracle_type.info = OracleTypeAnnotation::CAST_TO_VARCHAR;
		return LogicalType::VARCHAR;
	} else if (base_type == "BOOLEAN") {
		// Oracle 23c+ BOOLEAN; older Oracle lacks this
		return LogicalType::BOOLEAN;
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
	return KeywordHelper::WriteOptionallyQuoted(text, '"', false);
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

} // namespace duckdb
