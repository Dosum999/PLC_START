# Built only with OPC UA support. open62541 is linked privately into the
# plugin (symbols not exported) so it cannot clash with the daemon's copy.
logict_add_driver(opcua_client opcua_client.cpp ${PROJECT_SOURCE_DIR}/northbound/opcua/variant.cpp)
target_include_directories(opcua_client PRIVATE ${PROJECT_SOURCE_DIR}/northbound)
target_link_libraries(opcua_client PRIVATE open62541::open62541)
target_link_options(opcua_client PRIVATE -Wl,--exclude-libs,ALL)
