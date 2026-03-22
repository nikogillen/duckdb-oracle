add_library(
  oracle_storage_library OBJECT
  oracle_catalog.cpp
  oracle_catalog_set.cpp
  oracle_clear_cache.cpp
  oracle_connection_pool.cpp
  oracle_delete.cpp
  oracle_index.cpp
  oracle_index_entry.cpp
  oracle_index_set.cpp
  oracle_insert.cpp
  oracle_merge_into.cpp
  oracle_optimizer.cpp
  oracle_schema_entry.cpp
  oracle_schema_set.cpp
  oracle_table_entry.cpp
  oracle_table_set.cpp
  oracle_view_entry.cpp
  oracle_transaction.cpp
  oracle_transaction_manager.cpp
  oracle_type_entry.cpp
  oracle_type_set.cpp
  oracle_update.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:oracle_storage_library>
    PARENT_SCOPE)
