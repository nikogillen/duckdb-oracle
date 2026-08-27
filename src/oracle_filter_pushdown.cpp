#include "oracle_filter_pushdown.hpp"
#include "oracle_utils.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

static string TransformFilter(const string &col_name, TableFilter &filter);

static string TransformConstantFilter(const string &col_name, ConstantFilter &filter) {
	string op;
	switch (filter.comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		op = "=";
		break;
	case ExpressionType::COMPARE_NOTEQUAL:
		op = "<>";
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		op = "<";
		break;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		op = "<=";
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		op = ">";
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		op = ">=";
		break;
	default:
		return string();
	}
	auto &val = filter.constant;
	string val_str;
	if (val.IsNull()) {
		if (filter.comparison_type == ExpressionType::COMPARE_EQUAL) {
			return OracleUtils::QuoteIdentifier(col_name) + " IS NULL";
		}
		return string();
	}
	switch (val.type().id()) {
	case LogicalTypeId::VARCHAR:
		val_str = OracleUtils::WriteLiteral(StringValue::Get(val));
		break;
	case LogicalTypeId::DATE: {
		auto d = DateValue::Get(val);
		int32_t y, mo, day;
		Date::Convert(d, y, mo, day);
		val_str = StringUtil::Format("DATE '%04d-%02d-%02d'", y, mo, day);
		break;
	}
	case LogicalTypeId::TIMESTAMP: {
		auto ts = TimestampValue::Get(val);
		auto d = Timestamp::GetDate(ts);
		auto t = Timestamp::GetTime(ts);
		int32_t y, mo, day, h, m, s, micros;
		Date::Convert(d, y, mo, day);
		Time::Convert(t, h, m, s, micros);
		val_str = StringUtil::Format("TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d.%06d'",
		                              y, mo, day, h, m, s, micros);
		break;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		// Push as UTC literal — Oracle TIMESTAMP WITH TIME ZONE accepts AT TIME ZONE 'UTC'
		auto ts = TimestampValue::Get(val);
		auto d = Timestamp::GetDate(ts);
		auto t = Timestamp::GetTime(ts);
		int32_t y, mo, day, h, m, s, micros;
		Date::Convert(d, y, mo, day);
		Time::Convert(t, h, m, s, micros);
		val_str = StringUtil::Format(
		    "TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d.%06d' AT TIME ZONE 'UTC'",
		    y, mo, day, h, m, s, micros);
		break;
	}
	default:
		// Route every other type through ValueToOracleSQL, which escapes string-like
		// values via WriteLiteral and emits numeric values unquoted. Never concatenate
		// a raw ToString() here — it would allow SQL injection through filter values.
		val_str = OracleUtils::ValueToOracleSQL(val);
		break;
	}
	return OracleUtils::QuoteIdentifier(col_name) + " " + op + " " + val_str;
}

static string TransformFilter(const string &col_name, TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &cf = filter.Cast<ConstantFilter>();
		return TransformConstantFilter(col_name, cf);
	}
	case TableFilterType::IS_NULL:
		return OracleUtils::QuoteIdentifier(col_name) + " IS NULL";
	case TableFilterType::IS_NOT_NULL:
		return OracleUtils::QuoteIdentifier(col_name) + " IS NOT NULL";
	case TableFilterType::CONJUNCTION_AND: {
		// Dropping a conjunct we cannot translate is safe: it only WIDENS the
		// predicate, so Oracle returns a superset and DuckDB filters the rest.
		auto &conj = filter.Cast<ConjunctionAndFilter>();
		vector<string> parts;
		for (auto &child : conj.child_filters) {
			auto s = TransformFilter(col_name, *child);
			if (!s.empty()) {
				parts.push_back(s);
			}
		}
		if (parts.empty()) {
			return string();
		}
		return "(" + StringUtil::Join(parts, " AND ") + ")";
	}
	case TableFilterType::CONJUNCTION_OR: {
		// The opposite of AND: dropping a disjunct NARROWS the predicate and would
		// lose rows. DuckDB removes a filter it considers fully pushed down from the
		// plan, so nothing would filter those rows back in. Give up on the whole OR
		// unless every branch translates.
		auto &conj = filter.Cast<ConjunctionOrFilter>();
		vector<string> parts;
		for (auto &child : conj.child_filters) {
			auto s = TransformFilter(col_name, *child);
			if (s.empty()) {
				return string();
			}
			parts.push_back(s);
		}
		if (parts.empty()) {
			return string();
		}
		return "(" + StringUtil::Join(parts, " OR ") + ")";
	}
	default:
		return string();
	}
}

//! Whether a filter on this column may be sent to Oracle at all. Being wrong in the
//! permissive direction is not a slow query but a failing one, so anything that is
//! not a plainly comparable scalar stays with DuckDB.
static bool CanPushFilterOnColumn(const OracleType &oracle_type, const LogicalType &duck_type) {
	switch (oracle_type.info) {
	case OracleTypeAnnotation::CLOB_AS_VARCHAR:
		// ORA-22848: a CLOB/NCLOB/LONG cannot be used as a comparison key. Note that
		// DuckDB VARCHAR columns created through this extension are Oracle CLOBs, so
		// this is the common case, not an exotic one.
		return false;
	case OracleTypeAnnotation::CAST_TO_VARCHAR:
		// The projection casts these to VARCHAR; a filter would compare the raw
		// column (XMLTYPE, INTERVAL, ...) against a text literal instead.
		return false;
	case OracleTypeAnnotation::JSON_AS_JSON:
	case OracleTypeAnnotation::VECTOR_AS_LIST:
	case OracleTypeAnnotation::SPATIAL_AS_GEOMETRY:
		// Read through JSON_SERIALIZE / SDO_UTIL.TO_WKTGEOMETRY / vector decoding —
		// the stored value is not comparable to the serialized form DuckDB filters on.
		return false;
	default:
		break;
	}
	// BLOB, RAW and LONG RAW all arrive as BLOB. Oracle accepts RAW as a comparison
	// key but not BLOB, and they are indistinguishable here, so stay conservative.
	return duck_type.id() != LogicalTypeId::BLOB;
}

string OracleFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                               optional_ptr<TableFilterSet> filters,
                                               const vector<string> &names,
                                               const vector<OracleType> &oracle_types,
                                               const vector<LogicalType> &duck_types) {
	if (!filters || filters->filters.empty()) {
		return string();
	}
	vector<string> filter_parts;
	for (auto &entry : filters->filters) {
		auto col_idx = column_ids[entry.first];
		// ROWID and any other virtual column has no counterpart in `names`. Skipping
		// is safe here because the parts are joined with AND (see above).
		if (col_idx == COLUMN_IDENTIFIER_ROW_ID || col_idx >= names.size()) {
			continue;
		}
		// Without type information we cannot rule out a LOB, so leave it to DuckDB.
		if (col_idx >= oracle_types.size() || col_idx >= duck_types.size() ||
		    !CanPushFilterOnColumn(oracle_types[col_idx], duck_types[col_idx])) {
			continue;
		}
		const auto &col_name = names[col_idx];
		auto s = TransformFilter(col_name, *entry.second);
		if (!s.empty()) {
			filter_parts.push_back(s);
		}
	}
	if (filter_parts.empty()) {
		return string();
	}
	return StringUtil::Join(filter_parts, " AND ");
}

} // namespace duckdb
