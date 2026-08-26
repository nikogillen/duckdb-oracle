#include "oracle_conversion.hpp"
#include "oracle_utils.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/decimal.hpp"
#if ORACLE_HAS_GEOMETRY_TYPE
#include "duckdb/common/types/geometry.hpp"
#endif

#include <cstring>
#include <cstdio>

namespace duckdb {

// Convert a dpiTimestamp to DuckDB timestamp (microseconds since epoch)
static timestamp_t OdpiTimestampToTimestamp(const dpiTimestamp &ts) {
	date_t d = Date::FromDate(ts.year, ts.month, ts.day);
	dtime_t t = Time::FromTime(ts.hour, ts.minute, ts.second,
	                            (int32_t)(ts.fsecond / 1000)); // nanosec -> microsec
	return Timestamp::FromDatetime(d, t);
}

// Convert a dpiTimestamp WITH TIME ZONE to DuckDB timestamp_tz_t
static timestamp_tz_t OdpiTimestampTZToTimestampTZ(const dpiTimestamp &ts) {
	// ODPI-C gives UTC offset in hours/minutes - convert to UTC first
	date_t d = Date::FromDate(ts.year, ts.month, ts.day);
	dtime_t t = Time::FromTime(ts.hour, ts.minute, ts.second,
	                            (int32_t)(ts.fsecond / 1000));
	timestamp_t utc_ts = Timestamp::FromDatetime(d, t);
	// Subtract the UTC offset
	int32_t offset_minutes = ts.tzHourOffset * 60 + ts.tzMinuteOffset;
	utc_ts = Timestamp::FromEpochMicroSeconds(
	    Timestamp::GetEpochMicroSeconds(utc_ts) - (int64_t)offset_minutes * 60 * 1e6);
	return timestamp_tz_t(Timestamp::GetEpochMicroSeconds(utc_ts));
}

// Read a character LOB (CLOB/NCLOB) fully into a string.
static string OracleReadClob(dpiLob *lob) {
	if (!lob) {
		return string();
	}
	uint64_t char_size = 0;
	if (dpiLob_getSize(lob, &char_size) < 0 || char_size == 0) {
		return string();
	}
	// getSize is in characters for character LOBs; allow up to 4 bytes/char (UTF-8).
	uint64_t buf_len = char_size * 4;
	string result;
	result.resize(buf_len);
	uint64_t read_len = buf_len;
	if (dpiLob_readBytes(lob, 1, char_size, &result[0], &read_len) < 0) {
		return string();
	}
	result.resize(read_len);
	return result;
}

// Read a binary LOB (BLOB) fully into a string buffer.
static string OracleReadBlob(dpiLob *lob) {
	if (!lob) {
		return string();
	}
	uint64_t byte_size = 0;
	if (dpiLob_getSize(lob, &byte_size) < 0 || byte_size == 0) {
		return string();
	}
	string result;
	result.resize(byte_size);
	uint64_t read_len = byte_size;
	if (dpiLob_readBytes(lob, 1, byte_size, &result[0], &read_len) < 0) {
		return string();
	}
	result.resize(read_len);
	return result;
}

// Parse an Oracle NUMBER string into the unscaled integer DuckDB stores for a
// DECIMAL of the given scale, e.g. "12.345" with scale 2 -> 1235 (half-up).
// Handles a leading sign, an optional exponent, and digit counts beyond what
// double or int64 could hold. Returns false if the text is not a number.
static bool OracleParseDecimal(const string &text, uint8_t scale, hugeint_t &result) {
	idx_t pos = 0;
	while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
		pos++;
	}
	bool negative = false;
	if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
		negative = text[pos] == '-';
		pos++;
	}

	hugeint_t digits(0);
	int64_t exponent = 0; // decimal places already consumed
	bool any_digit = false;
	bool seen_dot = false;
	for (; pos < text.size(); pos++) {
		char c = text[pos];
		if (c == '.') {
			if (seen_dot) {
				return false;
			}
			seen_dot = true;
			continue;
		}
		if (c == 'e' || c == 'E') {
			break;
		}
		if (c < '0' || c > '9') {
			return false;
		}
		any_digit = true;
		digits = Hugeint::Add(Hugeint::Multiply(digits, hugeint_t(10)), hugeint_t(c - '0'));
		if (seen_dot) {
			exponent++;
		}
	}
	if (!any_digit) {
		return false;
	}
	if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
		// Oracle may return scientific notation for very large/small magnitudes.
		int64_t exp_value = 0;
		bool exp_negative = false;
		pos++;
		if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
			exp_negative = text[pos] == '-';
			pos++;
		}
		bool exp_digit = false;
		for (; pos < text.size(); pos++) {
			if (text[pos] < '0' || text[pos] > '9') {
				return false;
			}
			exp_digit = true;
			exp_value = exp_value * 10 + (text[pos] - '0');
			if (exp_value > 1000) { // far outside any DECIMAL range
				return false;
			}
		}
		if (!exp_digit) {
			return false;
		}
		exponent -= exp_negative ? -exp_value : exp_value;
	}

	// Rescale to the target scale.
	int64_t shift = (int64_t)scale - exponent;
	if (shift > 0) {
		for (int64_t i = 0; i < shift; i++) {
			digits = Hugeint::Multiply(digits, hugeint_t(10));
		}
	} else if (shift < 0) {
		// Drop excess decimals, rounding half away from zero on the last one.
		for (int64_t i = 0; i < -shift - 1; i++) {
			digits = Hugeint::Divide(digits, hugeint_t(10));
		}
		auto last = Hugeint::Modulo(digits, hugeint_t(10));
		digits = Hugeint::Divide(digits, hugeint_t(10));
		if (Hugeint::Cast<int64_t>(last) >= 5) {
			digits = Hugeint::Add(digits, hugeint_t(1));
		}
	}
	result = negative ? Hugeint::Negate(digits) : digits;
	return true;
}

// Append a JSON-escaped string literal (with surrounding quotes) to out.
static void OracleJsonEscape(const char *s, uint32_t len, string &out) {
	out += '"';
	for (uint32_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += (char)c;
			}
		}
	}
	out += '"';
}

// Recursively serialize an ODPI-C JSON node to compact JSON text.
static void OracleSerializeJsonNode(const dpiJsonNode *node, string &out) {
	if (!node || !node->value) {
		out += "null";
		return;
	}
	switch (node->nativeTypeNum) {
	case DPI_NATIVE_TYPE_JSON_OBJECT: {
		const dpiJsonObject &obj = node->value->asJsonObject;
		out += '{';
		for (uint32_t i = 0; i < obj.numFields; i++) {
			if (i) out += ',';
			OracleJsonEscape(obj.fieldNames[i], obj.fieldNameLengths[i], out);
			out += ':';
			OracleSerializeJsonNode(&obj.fields[i], out);
		}
		out += '}';
		break;
	}
	case DPI_NATIVE_TYPE_JSON_ARRAY: {
		const dpiJsonArray &arr = node->value->asJsonArray;
		out += '[';
		for (uint32_t i = 0; i < arr.numElements; i++) {
			if (i) out += ',';
			OracleSerializeJsonNode(&arr.elements[i], out);
		}
		out += ']';
		break;
	}
	case DPI_NATIVE_TYPE_BYTES:
		// Numbers are requested as strings (see DPI_JSON_OPT_NUMBER_AS_STRING) and
		// emitted unquoted; everything else is a JSON string.
		if (node->oracleTypeNum == DPI_ORACLE_TYPE_NUMBER) {
			out.append(node->value->asBytes.ptr, node->value->asBytes.length);
		} else {
			OracleJsonEscape(node->value->asBytes.ptr, node->value->asBytes.length, out);
		}
		break;
	case DPI_NATIVE_TYPE_INT64:
		out += to_string(node->value->asInt64);
		break;
	case DPI_NATIVE_TYPE_DOUBLE: {
		char buf[32];
		snprintf(buf, sizeof(buf), "%.17g", node->value->asDouble);
		out += buf;
		break;
	}
	case DPI_NATIVE_TYPE_BOOLEAN:
		out += node->value->asBoolean ? "true" : "false";
		break;
	default:
		out += "null";
		break;
	}
}

void OracleConvertValue(Vector &col, idx_t out_row,
                         dpiData *data, dpiNativeTypeNum native_type,
                         const LogicalType &target_type,
                         const OracleType &oracle_type) {
	if (data->isNull) {
		FlatVector::SetNull(col, out_row, true);
		return;
	}

	auto target_id = target_type.id();

	switch (target_id) {
	case LogicalTypeId::BOOLEAN: {
		bool val = false;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = data->value.asInt64 != 0;
		} else if (native_type == DPI_NATIVE_TYPE_BOOLEAN) {
			val = data->value.asBoolean != 0;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			string s(data->value.asBytes.ptr, data->value.asBytes.length);
			val = (s == "1" || s == "Y" || s == "y" || s == "TRUE" || s == "true");
		}
		FlatVector::GetData<bool>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::TINYINT: {
		int8_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (int8_t)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int8_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int8_t)atoi(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetData<int8_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::SMALLINT: {
		int16_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (int16_t)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int16_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int16_t)atoi(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetData<int16_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::INTEGER: {
		int32_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (int32_t)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int32_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int32_t)atoi(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetData<int32_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::BIGINT: {
		int64_t val = 0;
		if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (int64_t)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (int64_t)atoll(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetData<int64_t>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::FLOAT: {
		float val = 0.0f;
		if (native_type == DPI_NATIVE_TYPE_FLOAT) {
			val = data->value.asFloat;
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = (float)data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = (float)atof(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetData<float>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::DOUBLE: {
		double val = 0.0;
		if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			val = data->value.asDouble;
		} else if (native_type == DPI_NATIVE_TYPE_FLOAT) {
			val = (double)data->value.asFloat;
		} else if (native_type == DPI_NATIVE_TYPE_INT64) {
			val = (double)data->value.asInt64;
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			val = atof(string(data->value.asBytes.ptr, data->value.asBytes.length).c_str());
		}
		FlatVector::GetData<double>(col)[out_row] = val;
		break;
	}
	case LogicalTypeId::DECIMAL: {
		// Oracle NUMBER comes back as DPI_NATIVE_TYPE_BYTES (string repr) by default.
		// Parse the digits exactly instead of going through double: a DECIMAL(38,x)
		// carries up to 38 digits, which neither double (~15-16 digits) nor the
		// intermediate int64 can represent — that silently corrupted large values.
		auto width = DecimalType::GetWidth(target_type);
		auto scale = DecimalType::GetScale(target_type);

		string text;
		if (native_type == DPI_NATIVE_TYPE_BYTES) {
			text = string(data->value.asBytes.ptr, data->value.asBytes.length);
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			text = StringUtil::Format("%.17g", data->value.asDouble);
		} else if (native_type == DPI_NATIVE_TYPE_INT64) {
			text = to_string(data->value.asInt64);
		}

		hugeint_t unscaled(0);
		bool parsed = OracleParseDecimal(text, scale, unscaled);
		if (!parsed) {
			FlatVector::SetNull(col, out_row, true);
			break;
		}
		if (width <= Decimal::MAX_WIDTH_INT16) {
			FlatVector::GetData<int16_t>(col)[out_row] = (int16_t)Hugeint::Cast<int64_t>(unscaled);
		} else if (width <= Decimal::MAX_WIDTH_INT32) {
			FlatVector::GetData<int32_t>(col)[out_row] = (int32_t)Hugeint::Cast<int64_t>(unscaled);
		} else if (width <= Decimal::MAX_WIDTH_INT64) {
			FlatVector::GetData<int64_t>(col)[out_row] = Hugeint::Cast<int64_t>(unscaled);
		} else {
			FlatVector::GetData<hugeint_t>(col)[out_row] = unscaled;
		}
		break;
	}
	case LogicalTypeId::VARCHAR: {
		string str_val;
		if (native_type == DPI_NATIVE_TYPE_BYTES) {
			str_val = string(data->value.asBytes.ptr, data->value.asBytes.length);
		} else if (native_type == DPI_NATIVE_TYPE_TIMESTAMP) {
			auto &ts = data->value.asTimestamp;
			char buf[64];
			snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
			         ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.second,
			         ts.fsecond / 1000);
			str_val = buf;
		} else if (native_type == DPI_NATIVE_TYPE_INT64) {
			str_val = to_string(data->value.asInt64);
		} else if (native_type == DPI_NATIVE_TYPE_DOUBLE) {
			str_val = to_string(data->value.asDouble);
		} else if (native_type == DPI_NATIVE_TYPE_INTERVAL_DS) {
			auto &ivl = data->value.asIntervalDS;
			char buf[64];
			snprintf(buf, sizeof(buf), "%d %02d:%02d:%02d.%06d", ivl.days,
			         ivl.hours, ivl.minutes, ivl.seconds, ivl.fseconds / 1000);
			str_val = buf;
		} else if (native_type == DPI_NATIVE_TYPE_INTERVAL_YM) {
			auto &ivl = data->value.asIntervalYM;
			char buf[32];
			snprintf(buf, sizeof(buf), "%d-%d", ivl.years, ivl.months);
			str_val = buf;
		} else if (native_type == DPI_NATIVE_TYPE_LOB) {
			// CLOB/NCLOB come back as a LOB handle under the default fetch. Read the
			// whole value into memory. dpiLob_getSize returns the length in characters
			// for character LOBs; UTF-8 needs up to 4 bytes per character.
			str_val = OracleReadClob(data->value.asLOB);
		} else if (native_type == DPI_NATIVE_TYPE_JSON) {
			// Oracle native JSON → serialize to compact JSON text (target is DuckDB JSON,
			// which is VARCHAR under the hood). Numbers as strings to preserve precision.
			dpiJsonNode *node = nullptr;
			if (dpiJson_getValue(data->value.asJson, DPI_JSON_OPT_NUMBER_AS_STRING,
			                     &node) == 0) {
				OracleSerializeJsonNode(node, str_val);
			}
		}
		FlatVector::GetData<string_t>(col)[out_row] =
		    StringVector::AddString(col, str_val);
		break;
	}
#if ORACLE_HAS_GEOMETRY_TYPE
	case LogicalTypeId::GEOMETRY: {
		// SDO_GEOMETRY arrives as WKT text (the scan wraps the column in
		// SDO_UTIL.TO_WKTGEOMETRY, which returns a CLOB).
		string wkt;
		if (native_type == DPI_NATIVE_TYPE_LOB) {
			wkt = OracleReadClob(data->value.asLOB);
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			wkt = string(data->value.asBytes.ptr, data->value.asBytes.length);
		}
		if (wkt.empty()) {
			FlatVector::SetNull(col, out_row, true);
			break;
		}
		string_t geom;
		if (!Geometry::FromString(string_t(wkt.c_str(), (uint32_t)wkt.size()), geom, col,
		                          false)) {
			// Not parseable as WKT — surface NULL rather than failing the whole scan.
			FlatVector::SetNull(col, out_row, true);
			break;
		}
		FlatVector::GetData<string_t>(col)[out_row] = geom;
		break;
	}
#endif
	case LogicalTypeId::LIST: {
		// Oracle 23ai VECTOR → LIST(FLOAT).
		if (native_type == DPI_NATIVE_TYPE_VECTOR) {
			dpiVectorInfo info;
			if (dpiVector_getValue(data->value.asVector, &info) == 0 && !info.isSparse) {
				idx_t start = ListVector::GetListSize(col);
				ListVector::Reserve(col, start + info.numDimensions);
				auto child_data = FlatVector::GetData<float>(ListVector::GetEntry(col));
				for (uint32_t i = 0; i < info.numDimensions; i++) {
					float v = 0.0f;
					switch (info.format) {
					case DPI_VECTOR_FORMAT_FLOAT32: v = info.dimensions.asFloat[i]; break;
					case DPI_VECTOR_FORMAT_FLOAT64: v = (float)info.dimensions.asDouble[i]; break;
					case DPI_VECTOR_FORMAT_INT8: v = (float)info.dimensions.asInt8[i]; break;
					default: v = 0.0f; break; // BINARY unsupported → zeros
					}
					child_data[start + i] = v;
				}
				ListVector::SetListSize(col, start + info.numDimensions);
				auto list_entries = FlatVector::GetData<list_entry_t>(col);
				list_entries[out_row].offset = start;
				list_entries[out_row].length = info.numDimensions;
			} else {
				FlatVector::SetNull(col, out_row, true);
			}
		} else {
			FlatVector::SetNull(col, out_row, true);
		}
		break;
	}
	case LogicalTypeId::BLOB: {
		string blob_val;
		if (native_type == DPI_NATIVE_TYPE_BYTES) {
			blob_val = string(data->value.asBytes.ptr, data->value.asBytes.length);
		} else if (native_type == DPI_NATIVE_TYPE_LOB) {
			// BLOB comes back as a LOB handle; dpiLob_getSize returns bytes here.
			blob_val = OracleReadBlob(data->value.asLOB);
		}
		FlatVector::GetData<string_t>(col)[out_row] =
		    StringVector::AddStringOrBlob(col, blob_val);
		break;
	}
	case LogicalTypeId::DATE: {
		date_t d;
		if (native_type == DPI_NATIVE_TYPE_TIMESTAMP) {
			d = Date::FromDate(data->value.asTimestamp.year,
			                   data->value.asTimestamp.month,
			                   data->value.asTimestamp.day);
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			// ISO format string
			string s(data->value.asBytes.ptr, data->value.asBytes.length);
			bool special;
			d = Date::FromCString(s.c_str(), s.size(), special);
		} else {
			d = Date::FromDate(1970, 1, 1);
		}
		FlatVector::GetData<date_t>(col)[out_row] = d;
		break;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		timestamp_t ts;
		if (native_type == DPI_NATIVE_TYPE_TIMESTAMP) {
			if (target_id == LogicalTypeId::TIMESTAMP_TZ) {
				auto tz = OdpiTimestampTZToTimestampTZ(data->value.asTimestamp);
				FlatVector::GetData<timestamp_tz_t>(col)[out_row] = tz;
				return;
			}
			ts = OdpiTimestampToTimestamp(data->value.asTimestamp);
		} else if (native_type == DPI_NATIVE_TYPE_BYTES) {
			string s(data->value.asBytes.ptr, data->value.asBytes.length);
			bool special;
			ts = Timestamp::FromCString(s.c_str(), s.size());
		} else {
			ts = Timestamp::FromEpochMicroSeconds(0);
		}
		FlatVector::GetData<timestamp_t>(col)[out_row] = ts;
		break;
	}
	case LogicalTypeId::INTERVAL: {
		interval_t ivl = {};
		if (native_type == DPI_NATIVE_TYPE_INTERVAL_DS) {
			auto &ds = data->value.asIntervalDS;
			ivl.days = ds.days;
			ivl.micros = ((int64_t)ds.hours * 3600 + (int64_t)ds.minutes * 60 +
			               ds.seconds) * 1000000LL + ds.fseconds / 1000;
		} else if (native_type == DPI_NATIVE_TYPE_INTERVAL_YM) {
			// Approximate: months only
			auto &ym = data->value.asIntervalYM;
			ivl.months = ym.years * 12 + ym.months;
		}
		FlatVector::GetData<interval_t>(col)[out_row] = ivl;
		break;
	}
	default:
		FlatVector::SetNull(col, out_row, true);
		break;
	}
}

} // namespace duckdb
