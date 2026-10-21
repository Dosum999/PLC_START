# Built only when unixODBC (or another ODBC driver manager) is found.
add_library(logict_odbc_sql STATIC odbc_sql.cpp)
target_include_directories(logict_odbc_sql PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logict_odbc_sql PUBLIC logict::sdk ODBC::ODBC PRIVATE logict_warnings)
set_target_properties(logict_odbc_sql PROPERTIES CXX_VISIBILITY_PRESET hidden)

logict_add_driver(odbc_client odbc_client.cpp)
target_link_libraries(odbc_client PRIVATE logict_odbc_sql)
