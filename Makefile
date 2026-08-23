PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Extension identity and build configuration
EXT_NAME=oracle
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the standard DuckDB extension makefile. extension-ci-tools is checked out
# next to this Makefile by the CI (or as a sibling directory for local use).
include extension-ci-tools/makefiles/duckdb_extension.Makefile
