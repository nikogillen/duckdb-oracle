#include "duckdb.hpp"
#include "oracle_storage.hpp"
#include "storage/oracle_catalog.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "storage/oracle_transaction_manager.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"
#include "oracle_duckdb_compat.hpp"

namespace duckdb {

static unique_ptr<Catalog> OracleAttach(optional_ptr<StorageExtensionInfo> storage_info,
                                         ClientContext &context, AttachedDatabase &db,
                                         const string &name, AttachInfo &info,
                                         AttachOptions &attach_options) {
	// DuckDB 1.5 reads settings via Settings::Get<>; 1.4 LTS uses the direct member.
#if ORACLE_DUCKDB_15_PLUS
	bool external_access = Settings::Get<EnableExternalAccessSetting>(DBConfig::GetConfig(context));
#else
	bool external_access = DBConfig::GetConfig(context).options.enable_external_access;
#endif
	if (!external_access) {
		throw PermissionException(
		    "Attaching Oracle databases is disabled through configuration");
	}
	string attach_path = info.path;

	string secret_name;
	string schema_to_load;
	string config_dir;
	OracleIsolationLevel isolation_level = OracleIsolationLevel::READ_COMMITTED;
	OraclePrivilegeLevel privilege_level = OraclePrivilegeLevel::USER;

	for (auto &entry : attach_options.options) {
		auto lower_name = StringUtil::Lower(entry.first);
		if (lower_name == "secret") {
			secret_name = entry.second.ToString();
		} else if (lower_name == "schema") {
			schema_to_load = entry.second.ToString();
		} else if (lower_name == "config_dir" || lower_name == "tns_admin" ||
		           lower_name == "wallet_path") {
			// Oracle client configuration directory (TNS_ADMIN): tnsnames.ora,
			// sqlnet.ora and wallet files are resolved from here.
			config_dir = entry.second.ToString();
			if (!config_dir.empty() &&
			    !FileSystem::GetFileSystem(context).DirectoryExists(config_dir)) {
				throw InvalidInputException(
				    "Oracle %s \"%s\" is not an existing directory", lower_name, config_dir);
			}
		} else if (lower_name == "isolation_level") {
			auto param = StringUtil::Lower(entry.second.ToString());
			if (param == "read committed") {
				isolation_level = OracleIsolationLevel::READ_COMMITTED;
			} else if (param == "serializable") {
				isolation_level = OracleIsolationLevel::SERIALIZABLE;
			} else {
				throw InvalidInputException(
				    "Invalid isolation_level for Oracle: \"%s\" (use READ COMMITTED or SERIALIZABLE)",
				    entry.second.ToString());
			}
		} else if (lower_name == "privilege_level") {
			auto param = StringUtil::Lower(entry.second.ToString());
			if (param == "user") {
				privilege_level = OraclePrivilegeLevel::USER;
			} else if (param == "all") {
				privilege_level = OraclePrivilegeLevel::ALL;
			} else if (param == "dba") {
				privilege_level = OraclePrivilegeLevel::DBA;
			} else {
				throw InvalidInputException(
				    "Invalid privilege_level for Oracle: \"%s\" (use USER, ALL, or DBA)",
				    entry.second.ToString());
			}
		} else {
			throw BinderException("Unrecognized option for Oracle attach: %s", entry.first);
		}
	}

	auto connection_string =
	    OracleCatalog::GetConnectionString(context, attach_path, secret_name);
	if (config_dir.empty()) {
		// Fall back to the secret's config_dir; an explicit ATTACH option wins.
		config_dir = OracleCatalog::GetSecretConfigDir(context, secret_name);
	}
	return make_uniq<OracleCatalog>(db, std::move(connection_string),
	                                 std::move(attach_path), attach_options.access_mode,
	                                 std::move(schema_to_load), isolation_level,
	                                 privilege_level, context, std::move(config_dir));
}

static unique_ptr<TransactionManager>
OracleCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                AttachedDatabase &db, Catalog &catalog) {
	auto &oracle_catalog = catalog.Cast<OracleCatalog>();
	return make_uniq<OracleTransactionManager>(db, oracle_catalog);
}

OracleStorageExtension::OracleStorageExtension() {
	attach = OracleAttach;
	create_transaction_manager = OracleCreateTransactionManager;
}

} // namespace duckdb
