# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(oracle
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    DONT_LINK
    LOAD_TESTS
)

# json is used by the integration tests (json_extract_string on Oracle JSON columns).
# tpch/tpcds came from the extension template and are not used anywhere in this repo -
# building their data generators cost CI time for nothing.
duckdb_extension_load(json)
