include_directories(include)

add_subdirectory(storage)

add_library(
  oracle_ext_library OBJECT
  oracle_attach.cpp
  oracle_connection.cpp
  oracle_conversion.cpp
  oracle_execute.cpp
  oracle_extension.cpp
  oracle_filter_pushdown.cpp
  oracle_logging.cpp
  oracle_query.cpp
  oracle_scanner.cpp
  oracle_storage.cpp
  oracle_utils.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:oracle_ext_library>
    PARENT_SCOPE)
