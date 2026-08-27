#include "storage/oracle_insert.hpp"
#include "storage/oracle_catalog.hpp"
#include "storage/oracle_transaction.hpp"
#include "storage/oracle_table_entry.hpp"
#include "oracle_scanner.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

OracleInsert::OracleInsert(PhysicalPlan &physical_plan, LogicalOperator &op,
                             TableCatalogEntry &table,
                             physical_index_vector_t<idx_t> column_index_map_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      table(&table), schema(nullptr), column_index_map(std::move(column_index_map_p)) {
}

OracleInsert::OracleInsert(PhysicalPlan &physical_plan, LogicalOperator &op,
                             SchemaCatalogEntry &schema,
                             unique_ptr<BoundCreateTableInfo> info_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      table(nullptr), schema(&schema), info(std::move(info_p)) {
}

// ---------------------------------------------------------------------------
// ODPI-C helpers
// ---------------------------------------------------------------------------

//! Smallest byte buffer we ever allocate for a character/raw bind variable.
static constexpr uint32_t ORACLE_MIN_BIND_SIZE = 64;
//! Above this the buffer is switched to 32K, which makes ODPI-C bind the column
//! dynamically (piecewise) and therefore removes the length limit altogether.
static constexpr uint32_t ORACLE_MAX_STATIC_BIND_SIZE = 8192;
//! Any size beyond DPI_MAX_BASIC_BUFFER_SIZE (32767) turns the variable dynamic.
static constexpr uint32_t ORACLE_DYNAMIC_BIND_SIZE = 32768;
//! Maximum number of characters ODPI-C accepts for a NUMBER bound as text.
static constexpr uint32_t ORACLE_NUMBER_AS_TEXT_CHARS = 173;

static string GetOracleError() {
	dpiErrorInfo error_info;
	dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
	if (!error_info.message) {
		return "unknown ODPI-C error";
	}
	return string(error_info.message, error_info.messageLength);
}

static void ThrowOracleError(const string &action) {
	throw IOException("Oracle %s failed: %s", action, GetOracleError());
}

static void CheckOracleYear(int32_t year) {
	if (year < -4712 || year > 9999) {
		throw InvalidInputException(
		    "Year %d is out of range for an Oracle DATE/TIMESTAMP value", year);
	}
}

// ---------------------------------------------------------------------------
// Sink state
// ---------------------------------------------------------------------------

//! Describes how a single DuckDB column is bound as an ODPI-C array variable.
struct OracleBindPlan {
	LogicalTypeId type_id = LogicalTypeId::INVALID;
	dpiOracleTypeNum oracle_type = DPI_ORACLE_TYPE_VARCHAR;
	dpiNativeTypeNum native_type = DPI_NATIVE_TYPE_BYTES;
	PhysicalType physical_type = PhysicalType::INVALID;
	uint8_t width = 0;
	uint8_t scale = 0;
	//! Buffer size handed to dpiConn_newVar (bytes)
	uint32_t size = 1;
	//! Whether the required buffer size depends on the data of a chunk
	bool variable_size = false;
	//! Whether ODPI-C binds this variable dynamically (unbounded length)
	bool is_dynamic = false;
};

class OracleInsertGlobalState : public GlobalSinkState {
public:
	explicit OracleInsertGlobalState(OracleTableEntry &table)
	    : table(table), insert_count(0) {
	}
	~OracleInsertGlobalState() override;

	OracleTableEntry &table;
	idx_t insert_count;
	vector<string> insert_column_names;

	//! Array binding state, lazily set up on the first Sink() call
	bool binds_planned = false;
	bool can_bind = false;
	vector<OracleBindPlan> bind_plans;
	shared_ptr<OwnedOracleConnection> owned_connection;
	dpiStmt *stmt = nullptr;
	vector<dpiVar *> bind_vars;
	vector<dpiData *> bind_data;

	//! INSERT INTO t(c1,c2) VALUES( - prefix used by the literal fallback
	string BuildLiteralPrefix() const;
	//! INSERT INTO t(c1,c2) VALUES(:1,:2) - statement used for array binding
	string BuildBindSQL() const;

	//! Decide whether every column can be bound; sets can_bind
	void PlanBinds(DataChunk &chunk);
	//! Prepare the statement and allocate the bind variables (once)
	void PrepareStatement(shared_ptr<OwnedOracleConnection> connection_p);
	//! Grow the byte buffer of a column if the current chunk needs more room
	void EnsureBindSize(idx_t col, uint32_t required);

private:
	void CreateBindVar(idx_t col, uint32_t size);
};

OracleInsertGlobalState::~OracleInsertGlobalState() {
	for (auto &var : bind_vars) {
		if (var) {
			dpiVar_release(var);
		}
	}
	bind_vars.clear();
	if (stmt) {
		dpiStmt_release(stmt);
		stmt = nullptr;
	}
}

string OracleInsertGlobalState::BuildLiteralPrefix() const {
	string base = "INSERT INTO " +
	              OracleUtils::QuoteIdentifier(table.oracle_schema_name) + "." +
	              OracleUtils::QuoteIdentifier(table.name) + "(";
	for (idx_t c = 0; c < insert_column_names.size(); c++) {
		if (c > 0) base += ", ";
		base += OracleUtils::QuoteIdentifier(insert_column_names[c]);
	}
	base += ") VALUES(";
	return base;
}

string OracleInsertGlobalState::BuildBindSQL() const {
	string sql = BuildLiteralPrefix();
	for (idx_t c = 0; c < insert_column_names.size(); c++) {
		if (c > 0) sql += ", ";
		sql += ":" + to_string(c + 1);
	}
	sql += ")";
	return sql;
}

// ---------------------------------------------------------------------------
// Bind planning
// ---------------------------------------------------------------------------

//! Map a DuckDB type onto an ODPI-C variable. Returns false if the type cannot
//! be bound, in which case the whole sink falls back to literal SQL.
static bool PlanBindForType(const LogicalType &type, OracleBindPlan &plan) {
	plan.type_id = type.id();
	plan.physical_type = type.InternalType();
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		plan.oracle_type = DPI_ORACLE_TYPE_NUMBER;
		plan.native_type = DPI_NATIVE_TYPE_INT64;
		return true;
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		plan.oracle_type = DPI_ORACLE_TYPE_NUMBER;
		plan.native_type = DPI_NATIVE_TYPE_UINT64;
		return true;
	case LogicalTypeId::FLOAT:
		plan.oracle_type = DPI_ORACLE_TYPE_NATIVE_FLOAT;
		plan.native_type = DPI_NATIVE_TYPE_FLOAT;
		return true;
	case LogicalTypeId::DOUBLE:
		plan.oracle_type = DPI_ORACLE_TYPE_NATIVE_DOUBLE;
		plan.native_type = DPI_NATIVE_TYPE_DOUBLE;
		return true;
	case LogicalTypeId::DECIMAL:
		// Bound as decimal text so that no precision is lost on the way out
		plan.oracle_type = DPI_ORACLE_TYPE_NUMBER;
		plan.native_type = DPI_NATIVE_TYPE_BYTES;
		plan.width = DecimalType::GetWidth(type);
		plan.scale = DecimalType::GetScale(type);
		plan.size = ORACLE_NUMBER_AS_TEXT_CHARS;
		return true;
	case LogicalTypeId::VARCHAR:
		plan.oracle_type = DPI_ORACLE_TYPE_VARCHAR;
		plan.native_type = DPI_NATIVE_TYPE_BYTES;
		plan.size = ORACLE_MIN_BIND_SIZE;
		plan.variable_size = true;
		return true;
	case LogicalTypeId::UUID:
		plan.oracle_type = DPI_ORACLE_TYPE_VARCHAR;
		plan.native_type = DPI_NATIVE_TYPE_BYTES;
		plan.size = ORACLE_MIN_BIND_SIZE;
		return true;
	case LogicalTypeId::BLOB:
		plan.oracle_type = DPI_ORACLE_TYPE_RAW;
		plan.native_type = DPI_NATIVE_TYPE_BYTES;
		plan.size = ORACLE_MIN_BIND_SIZE;
		plan.variable_size = true;
		return true;
	case LogicalTypeId::DATE:
		plan.oracle_type = DPI_ORACLE_TYPE_DATE;
		plan.native_type = DPI_NATIVE_TYPE_TIMESTAMP;
		return true;
	case LogicalTypeId::TIMESTAMP:
		plan.oracle_type = DPI_ORACLE_TYPE_TIMESTAMP;
		plan.native_type = DPI_NATIVE_TYPE_TIMESTAMP;
		return true;
	case LogicalTypeId::TIMESTAMP_TZ:
		plan.oracle_type = DPI_ORACLE_TYPE_TIMESTAMP_TZ;
		plan.native_type = DPI_NATIVE_TYPE_TIMESTAMP;
		return true;
	case LogicalTypeId::INTERVAL:
		plan.oracle_type = DPI_ORACLE_TYPE_INTERVAL_DS;
		plan.native_type = DPI_NATIVE_TYPE_INTERVAL_DS;
		return true;
	default:
		return false;
	}
}

void OracleInsertGlobalState::PlanBinds(DataChunk &chunk) {
	binds_planned = true;
	can_bind = false;
	auto num_cols = insert_column_names.size();
	if (num_cols == 0 || chunk.ColumnCount() < num_cols) {
		return;
	}
	bind_plans.resize(num_cols);
	for (idx_t c = 0; c < num_cols; c++) {
		if (!PlanBindForType(chunk.data[c].GetType(), bind_plans[c])) {
			bind_plans.clear();
			return;
		}
	}
	can_bind = true;
}

void OracleInsertGlobalState::CreateBindVar(idx_t col, uint32_t size) {
	auto &plan = bind_plans[col];
	dpiVar *var = nullptr;
	dpiData *data = nullptr;
	if (dpiConn_newVar(owned_connection->connection, plan.oracle_type, plan.native_type,
	                    STANDARD_VECTOR_SIZE, size, 1, 0, nullptr, &var, &data) < 0) {
		ThrowOracleError("bind variable allocation");
	}
	if (dpiStmt_bindByPos(stmt, (uint32_t)col + 1, var) < 0) {
		auto message = GetOracleError();
		dpiVar_release(var);
		throw IOException("Oracle bind failed for column \"%s\": %s",
		                  insert_column_names[col], message);
	}
	if (bind_vars[col]) {
		dpiVar_release(bind_vars[col]);
	}
	bind_vars[col] = var;
	bind_data[col] = data;
	plan.size = size;
	plan.is_dynamic = size >= ORACLE_DYNAMIC_BIND_SIZE;
}

void OracleInsertGlobalState::PrepareStatement(shared_ptr<OwnedOracleConnection> connection_p) {
	if (stmt) {
		return;
	}
	owned_connection = std::move(connection_p);
	auto sql = BuildBindSQL();
	if (dpiConn_prepareStmt(owned_connection->connection, 0, sql.c_str(),
	                         (uint32_t)sql.size(), nullptr, 0, &stmt) < 0) {
		stmt = nullptr;
		throw IOException("Oracle prepare failed for statement \"%s\": %s", sql,
		                  GetOracleError());
	}
	bind_vars.resize(bind_plans.size(), nullptr);
	bind_data.resize(bind_plans.size(), nullptr);
	for (idx_t c = 0; c < bind_plans.size(); c++) {
		CreateBindVar(c, bind_plans[c].size);
	}
}

void OracleInsertGlobalState::EnsureBindSize(idx_t col, uint32_t required) {
	auto &plan = bind_plans[col];
	if (plan.is_dynamic || required <= plan.size) {
		return;
	}
	uint32_t new_size = plan.size;
	while (new_size < required && new_size < ORACLE_MAX_STATIC_BIND_SIZE) {
		new_size *= 2;
	}
	if (new_size < required) {
		// Hand the column over to ODPI-C's dynamic (piecewise) binding, which
		// has no length limit; from here on the variable is never resized again.
		new_size = ORACLE_DYNAMIC_BIND_SIZE;
	}
	CreateBindVar(col, new_size);
}

unique_ptr<GlobalSinkState> OracleInsert::GetGlobalSinkState(ClientContext &context) const {
	optional_ptr<OracleTableEntry> insert_table;
	if (!table) {
		auto &schema_ref = *schema.get_mutable();
		insert_table = &schema_ref.CreateTable(schema_ref.GetCatalogTransaction(context),
		                                        *info)->Cast<OracleTableEntry>();
	} else {
		insert_table = &table.get_mutable()->Cast<OracleTableEntry>();
	}

	auto result = make_uniq<OracleInsertGlobalState>(*insert_table);
	auto &columns = insert_table->GetColumns();

	if (!column_index_map.empty()) {
		vector<PhysicalIndex> col_indexes;
		col_indexes.resize(columns.LogicalColumnCount(),
		                    PhysicalIndex(DConstants::INVALID_INDEX));
		for (idx_t c = 0; c < column_index_map.size(); c++) {
			auto mapped = column_index_map[PhysicalIndex(c)];
			if (mapped != DConstants::INVALID_INDEX) {
				col_indexes[mapped] = PhysicalIndex(c);
			}
		}
		for (auto &idx : col_indexes) {
			if (idx.index != DConstants::INVALID_INDEX) {
				result->insert_column_names.push_back(
				    columns.GetColumn(idx).GetName());
			}
		}
	} else {
		// All columns
		for (auto &col : columns.Logical()) {
			result->insert_column_names.push_back(col.GetName());
		}
	}
	return std::move(result);
}

// ---------------------------------------------------------------------------
// Literal fallback: one INSERT statement per row
// ---------------------------------------------------------------------------

static string ValueToOracleSQL(const Value &val) {
	if (val.IsNull()) {
		return "NULL";
	}
	switch (val.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return BooleanValue::Get(val) ? "1" : "0";
	case LogicalTypeId::VARCHAR:
		return OracleUtils::WriteLiteral(StringValue::Get(val));
	case LogicalTypeId::DATE: {
		auto d = DateValue::Get(val);
		int32_t y, m, day;
		Date::Convert(d, y, m, day);
		return StringUtil::Format("DATE '%04d-%02d-%02d'", y, m, day);
	}
	case LogicalTypeId::TIMESTAMP: {
		auto ts = TimestampValue::Get(val);
		auto d = Timestamp::GetDate(ts);
		auto t = Timestamp::GetTime(ts);
		int32_t y, mo, day, h, mi, s, us;
		Date::Convert(d, y, mo, day);
		Time::Convert(t, h, mi, s, us);
		return StringUtil::Format(
		    "TIMESTAMP '%04d-%02d-%02d %02d:%02d:%02d.%06d'", y, mo, day, h, mi, s, us);
	}
	case LogicalTypeId::BLOB: {
		// Hex-encode raw bytes for Oracle HEXTORAW(...)
		auto raw = StringValue::Get(val);
		string hex;
		static const char hx[] = "0123456789ABCDEF";
		for (uint8_t b : raw) {
			hex += hx[b >> 4];
			hex += hx[b & 0xF];
		}
		return "HEXTORAW('" + hex + "')";
	}
	default:
		return OracleUtils::WriteLiteral(val.ToString());
	}
}

static void SinkLiteralRowByRow(ExecutionContext &context, DataChunk &chunk,
                                 OracleInsertGlobalState &gstate,
                                 OracleConnection &connection) {
	auto base_sql = gstate.BuildLiteralPrefix();
	idx_t num_cols = gstate.insert_column_names.size();

	for (idx_t row = 0; row < chunk.size(); row++) {
		string sql = base_sql;
		for (idx_t c = 0; c < num_cols; c++) {
			if (c > 0) sql += ", ";
			auto val = chunk.GetValue(c, row);
			sql += ValueToOracleSQL(val);
		}
		sql += ")";
		connection.Execute(context.client, sql);
	}
}

// ---------------------------------------------------------------------------
// Array binding
// ---------------------------------------------------------------------------

static uint32_t MaxByteLength(const UnifiedVectorFormat &format, idx_t count) {
	auto source = UnifiedVectorFormat::GetData<string_t>(format);
	uint32_t max_length = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			continue;
		}
		auto length = (uint32_t)source[idx].GetSize();
		if (length > max_length) {
			max_length = length;
		}
	}
	return max_length;
}

//! Copy a single integer column into the bind buffer
template <class T>
static void BindIntColumn(const UnifiedVectorFormat &format, idx_t count, dpiData *data) {
	auto source = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t i = 0; i < count; i++) {
		auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			data[i].isNull = 1;
			continue;
		}
		dpiData_setInt64(&data[i], (int64_t)source[idx]);
	}
}

template <class T>
static void BindUIntColumn(const UnifiedVectorFormat &format, idx_t count, dpiData *data) {
	auto source = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t i = 0; i < count; i++) {
		auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			data[i].isNull = 1;
			continue;
		}
		dpiData_setUint64(&data[i], (uint64_t)source[idx]);
	}
}

template <class T>
static void BindDecimalColumn(const UnifiedVectorFormat &format, idx_t count,
                               const OracleBindPlan &plan, dpiVar *var, dpiData *data) {
	auto source = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t i = 0; i < count; i++) {
		auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			data[i].isNull = 1;
			continue;
		}
		auto text = Decimal::ToString(source[idx], plan.width, plan.scale);
		if (dpiVar_setFromBytes(var, (uint32_t)i, text.c_str(), (uint32_t)text.size()) < 0) {
			ThrowOracleError("setting NUMBER bind value");
		}
	}
}

static void BindBytesColumn(const UnifiedVectorFormat &format, idx_t count,
                             dpiVar *var, dpiData *data) {
	auto source = UnifiedVectorFormat::GetData<string_t>(format);
	for (idx_t i = 0; i < count; i++) {
		auto idx = format.sel->get_index(i);
		if (!format.validity.RowIsValid(idx)) {
			data[i].isNull = 1;
			continue;
		}
		auto &value = source[idx];
		if (dpiVar_setFromBytes(var, (uint32_t)i, value.GetData(),
		                         (uint32_t)value.GetSize()) < 0) {
			ThrowOracleError("setting bind value");
		}
	}
}

static void BindTimestampMicros(int64_t micros, dpiData &target) {
	timestamp_t ts(micros);
	auto date = Timestamp::GetDate(ts);
	auto time = Timestamp::GetTime(ts);
	int32_t year, month, day, hour, minute, second, microsecond;
	Date::Convert(date, year, month, day);
	Time::Convert(time, hour, minute, second, microsecond);
	CheckOracleYear(year);
	dpiData_setTimestamp(&target, (int16_t)year, (uint8_t)month, (uint8_t)day,
	                     (uint8_t)hour, (uint8_t)minute, (uint8_t)second,
	                     (uint32_t)microsecond * 1000, 0, 0);
}

static void BindColumn(const OracleBindPlan &plan, const UnifiedVectorFormat &format,
                        idx_t count, dpiVar *var, dpiData *data) {
	switch (plan.type_id) {
	case LogicalTypeId::BOOLEAN: {
		auto source = UnifiedVectorFormat::GetData<bool>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			dpiData_setInt64(&data[i], source[idx] ? 1 : 0);
		}
		break;
	}
	case LogicalTypeId::TINYINT:
		BindIntColumn<int8_t>(format, count, data);
		break;
	case LogicalTypeId::SMALLINT:
		BindIntColumn<int16_t>(format, count, data);
		break;
	case LogicalTypeId::INTEGER:
		BindIntColumn<int32_t>(format, count, data);
		break;
	case LogicalTypeId::BIGINT:
		BindIntColumn<int64_t>(format, count, data);
		break;
	case LogicalTypeId::UTINYINT:
		BindUIntColumn<uint8_t>(format, count, data);
		break;
	case LogicalTypeId::USMALLINT:
		BindUIntColumn<uint16_t>(format, count, data);
		break;
	case LogicalTypeId::UINTEGER:
		BindUIntColumn<uint32_t>(format, count, data);
		break;
	case LogicalTypeId::UBIGINT:
		BindUIntColumn<uint64_t>(format, count, data);
		break;
	case LogicalTypeId::FLOAT: {
		auto source = UnifiedVectorFormat::GetData<float>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			dpiData_setFloat(&data[i], source[idx]);
		}
		break;
	}
	case LogicalTypeId::DOUBLE: {
		auto source = UnifiedVectorFormat::GetData<double>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			dpiData_setDouble(&data[i], source[idx]);
		}
		break;
	}
	case LogicalTypeId::DECIMAL:
		switch (plan.physical_type) {
		case PhysicalType::INT16:
			BindDecimalColumn<int16_t>(format, count, plan, var, data);
			break;
		case PhysicalType::INT32:
			BindDecimalColumn<int32_t>(format, count, plan, var, data);
			break;
		case PhysicalType::INT64:
			BindDecimalColumn<int64_t>(format, count, plan, var, data);
			break;
		default:
			BindDecimalColumn<hugeint_t>(format, count, plan, var, data);
			break;
		}
		break;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		BindBytesColumn(format, count, var, data);
		break;
	case LogicalTypeId::UUID: {
		auto source = UnifiedVectorFormat::GetData<hugeint_t>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			auto text = UUID::ToString(source[idx]);
			if (dpiVar_setFromBytes(var, (uint32_t)i, text.c_str(),
			                         (uint32_t)text.size()) < 0) {
				ThrowOracleError("setting UUID bind value");
			}
		}
		break;
	}
	case LogicalTypeId::DATE: {
		auto source = UnifiedVectorFormat::GetData<date_t>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			auto value = source[idx];
			if (!Date::IsFinite(value)) {
				throw InvalidInputException(
				    "Infinite DATE values cannot be written to Oracle");
			}
			int32_t year, month, day;
			Date::Convert(value, year, month, day);
			CheckOracleYear(year);
			dpiData_setTimestamp(&data[i], (int16_t)year, (uint8_t)month, (uint8_t)day,
			                     0, 0, 0, 0, 0, 0);
		}
		break;
	}
	case LogicalTypeId::TIMESTAMP: {
		auto source = UnifiedVectorFormat::GetData<timestamp_t>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			auto value = source[idx];
			if (!Timestamp::IsFinite(value)) {
				throw InvalidInputException(
				    "Infinite TIMESTAMP values cannot be written to Oracle");
			}
			BindTimestampMicros(value.value, data[i]);
		}
		break;
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		// The stored value is already UTC, so bind it with a zero UTC offset
		auto source = UnifiedVectorFormat::GetData<timestamp_tz_t>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			timestamp_t value(source[idx].value);
			if (!Timestamp::IsFinite(value)) {
				throw InvalidInputException(
				    "Infinite TIMESTAMP WITH TIME ZONE values cannot be written to Oracle");
			}
			BindTimestampMicros(value.value, data[i]);
		}
		break;
	}
	case LogicalTypeId::INTERVAL: {
		auto source = UnifiedVectorFormat::GetData<interval_t>(format);
		for (idx_t i = 0; i < count; i++) {
			auto idx = format.sel->get_index(i);
			if (!format.validity.RowIsValid(idx)) {
				data[i].isNull = 1;
				continue;
			}
			auto value = source[idx];
			if (value.months != 0) {
				throw NotImplementedException(
				    "INTERVAL values with a month component cannot be written to Oracle "
				    "INTERVAL DAY TO SECOND columns");
			}
			int64_t rest = (int64_t)value.days * Interval::MICROS_PER_DAY + value.micros;
			int32_t days = (int32_t)(rest / Interval::MICROS_PER_DAY);
			rest %= Interval::MICROS_PER_DAY;
			int32_t hours = (int32_t)(rest / Interval::MICROS_PER_HOUR);
			rest %= Interval::MICROS_PER_HOUR;
			int32_t minutes = (int32_t)(rest / Interval::MICROS_PER_MINUTE);
			rest %= Interval::MICROS_PER_MINUTE;
			int32_t seconds = (int32_t)(rest / Interval::MICROS_PER_SEC);
			rest %= Interval::MICROS_PER_SEC;
			dpiData_setIntervalDS(&data[i], days, hours, minutes, seconds,
			                      (int32_t)(rest * 1000));
		}
		break;
	}
	default:
		throw InternalException("Unsupported Oracle bind type %s",
		                        LogicalTypeIdToString(plan.type_id));
	}
}

// ---------------------------------------------------------------------------
// Sink
// ---------------------------------------------------------------------------

SinkResultType OracleInsert::Sink(ExecutionContext &context, DataChunk &chunk,
                                   OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<OracleInsertGlobalState>();
	auto &transaction = OracleTransaction::Get(context.client, gstate.table.catalog);
	auto &connection = transaction.GetConnection();

	auto count = chunk.size();
	if (count == 0) {
		return SinkResultType::NEED_MORE_INPUT;
	}

	if (!gstate.binds_planned) {
		gstate.PlanBinds(chunk);
	}
	if (!gstate.can_bind) {
		SinkLiteralRowByRow(context, chunk, gstate, connection);
		gstate.insert_count += count;
		return SinkResultType::NEED_MORE_INPUT;
	}

	auto num_cols = gstate.insert_column_names.size();
	auto owned = connection.GetConnection();
	lock_guard<mutex> guard(owned->connection_lock);
	gstate.PrepareStatement(std::move(owned));

	vector<UnifiedVectorFormat> formats(num_cols);
	for (idx_t c = 0; c < num_cols; c++) {
		chunk.data[c].ToUnifiedFormat(count, formats[c]);
		if (gstate.bind_plans[c].variable_size) {
			gstate.EnsureBindSize(c, MaxByteLength(formats[c], count));
		}
	}

	for (idx_t c = 0; c < num_cols; c++) {
		auto data = gstate.bind_data[c];
		BindColumn(gstate.bind_plans[c], formats[c], count, gstate.bind_vars[c], data);
		// ODPI-C transfers every slot of the array, so the unused tail has to
		// hold well-defined (NULL) values rather than data of a previous chunk
		for (idx_t i = count; i < STANDARD_VECTOR_SIZE; i++) {
			data[i].isNull = 1;
		}
	}

	// Never pass DPI_MODE_EXEC_COMMIT_ON_SUCCESS here: commit/rollback is driven
	// by OracleTransaction.
	if (dpiStmt_executeMany(gstate.stmt, DPI_MODE_EXEC_DEFAULT, (uint32_t)count) < 0) {
		dpiErrorInfo error_info;
		dpiContext_getError(OracleUtils::GetOrCreateContext(), &error_info);
		auto message = error_info.message
		                   ? string(error_info.message, error_info.messageLength)
		                   : string("unknown ODPI-C error");
		throw IOException(
		    "Oracle batch insert into \"%s\".\"%s\" failed at row %llu of the batch "
		    "(row %llu overall): %s",
		    gstate.table.oracle_schema_name, gstate.table.name,
		    (idx_t)error_info.offset + 1,
		    gstate.insert_count + (idx_t)error_info.offset + 1, message);
	}

	gstate.insert_count += count;
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType OracleInsert::Finalize(Pipeline &pipeline, Event &event,
                                         ClientContext &context,
                                         OperatorSinkFinalizeInput &input) const {
	// Transaction will be committed by the transaction manager
	return SinkFinalizeType::READY;
}

// ---------------------------------------------------------------------------
// Source (return count)
// ---------------------------------------------------------------------------

SourceResultType OracleInsert::ORACLE_GET_DATA_METHOD(ExecutionContext &context, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	auto &insert_gstate = sink_state->Cast<OracleInsertGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(insert_gstate.insert_count));
	return SourceResultType::FINISHED;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

string OracleInsert::GetName() const {
	return table ? "ORA_INSERT" : "ORA_CREATE_TABLE_AS";
}

InsertionOrderPreservingMap<string> OracleInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table Name"] = table ? table->name : info->Base().table;
	return result;
}

// ---------------------------------------------------------------------------
// Plan (called from OracleCatalog::PlanInsert / PlanCreateTableAs)
// ---------------------------------------------------------------------------

static PhysicalOperator &AddCastToOracleTypes(ClientContext &context,
                                               PhysicalPlanGenerator &planner,
                                               PhysicalOperator &plan) {
	bool require_cast = false;
	auto &child_types = plan.GetTypes();
	for (auto &type : child_types) {
		auto oracle_type = OracleUtils::ToOracleType(type);
		if (oracle_type != type) {
			require_cast = true;
			break;
		}
	}
	if (!require_cast) {
		return plan;
	}
	vector<LogicalType> oracle_types;
	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &type = child_types[i];
		unique_ptr<Expression> expr = make_uniq<BoundReferenceExpression>(type, i);
		auto oracle_type = OracleUtils::ToOracleType(type);
		if (oracle_type != type) {
			expr = BoundCastExpression::AddCastToType(context, std::move(expr), oracle_type);
		}
		oracle_types.push_back(std::move(oracle_type));
		select_list.push_back(std::move(expr));
	}
	auto &proj = planner.Make<PhysicalProjection>(std::move(oracle_types),
	                                               std::move(select_list),
	                                               plan.estimated_cardinality);
	proj.children.push_back(plan);
	return proj;
}

PhysicalOperator &OracleCatalog::PlanInsert(ClientContext &context,
                                             PhysicalPlanGenerator &planner,
                                             LogicalInsert &op,
                                             optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw BinderException(
		    "RETURNING clause not yet supported for Oracle table insertion");
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw BinderException("ON CONFLICT not yet supported for Oracle table insertion");
	}
	D_ASSERT(plan);
	MaterializeOracleScans(*plan);
	auto &inner_plan = AddCastToOracleTypes(context, planner, *plan);
	auto &insert = planner.Make<OracleInsert>(op, op.table, op.column_index_map);
	insert.children.push_back(inner_plan);
	return insert;
}

PhysicalOperator &OracleCatalog::PlanCreateTableAs(ClientContext &context,
                                                     PhysicalPlanGenerator &planner,
                                                     LogicalCreateTable &op,
                                                     PhysicalOperator &plan) {
	auto &inner_plan = AddCastToOracleTypes(context, planner, plan);
	MaterializeOracleScans(inner_plan);
	auto &insert = planner.Make<OracleInsert>(op, op.schema, std::move(op.info));
	insert.children.push_back(inner_plan);
	return insert;
}

} // namespace duckdb
