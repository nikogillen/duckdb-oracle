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
		val_str = val.ToString();
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
		auto &conj = filter.Cast<ConjunctionOrFilter>();
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
		return "(" + StringUtil::Join(parts, " OR ") + ")";
	}
	default:
		return string();
	}
}

string OracleFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                               optional_ptr<TableFilterSet> filters,
                                               const vector<string> &names) {
	if (!filters || filters->filters.empty()) {
		return string();
	}
	vector<string> filter_parts;
	for (auto &entry : filters->filters) {
		auto col_idx = column_ids[entry.first];
		if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
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
