//===----------------------------------------------------------------------===//
//                         DuckDB
//
// oracle_scanner_extension.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class OracleScannerExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override {
		return "oracle_scanner";
	}
};

} // namespace duckdb
