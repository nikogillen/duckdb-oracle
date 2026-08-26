//===----------------------------------------------------------------------===//
// oracle_duckdb_compat.hpp
//
// Compile-time detection of the DuckDB API generation so the extension builds
// against both the 1.4 LTS and 1.5+ lines.
//
// ORACLE_DUCKDB_15_PLUS == 1  -> DuckDB 1.5+ APIs (ExtensionCallbackManager,
//                                Settings::Get<...>, etc.)
// ORACLE_DUCKDB_15_PLUS == 0  -> DuckDB 1.4 LTS APIs (direct DBConfig members)
//
// Detection uses a header that only exists on 1.5+.
//===----------------------------------------------------------------------===//

#pragma once

#if defined(__has_include)
#  if __has_include("duckdb/main/extension_callback_manager.hpp")
#    define ORACLE_DUCKDB_15_PLUS 1
#  else
#    define ORACLE_DUCKDB_15_PLUS 0
#  endif
#else
#  define ORACLE_DUCKDB_15_PLUS 0
#endif

// The virtual source method on PhysicalOperator was renamed from GetData (1.4)
// to GetDataInternal (1.5, with GetData kept as a non-virtual wrapper).
#if ORACLE_DUCKDB_15_PLUS
#  define ORACLE_GET_DATA_METHOD GetDataInternal
#else
#  define ORACLE_GET_DATA_METHOD GetData
#endif

// A native GEOMETRY logical type (with optional CRS) exists only from DuckDB 1.5
// on. On 1.4 LTS, Oracle SDO_GEOMETRY is surfaced as WKT text in a VARCHAR.
#if defined(__has_include)
#  if __has_include("duckdb/common/types/geometry_crs.hpp")
#    define ORACLE_HAS_GEOMETRY_TYPE 1
#  else
#    define ORACLE_HAS_GEOMETRY_TYPE 0
#  endif
#else
#  define ORACLE_HAS_GEOMETRY_TYPE 0
#endif
