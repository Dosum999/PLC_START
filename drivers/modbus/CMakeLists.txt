# Protocol and device code shared by the Modbus TCP and RTU drivers, and unit tests.
add_library(logict_modbus_protocol STATIC protocol.cpp modbus_device.cpp server.cpp)
target_include_directories(logict_modbus_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logict_modbus_protocol PUBLIC logict::sdk PRIVATE logict_warnings)
set_target_properties(logict_modbus_protocol PROPERTIES CXX_VISIBILITY_PRESET hidden)

logict_add_driver(modbus_tcp modbus_tcp.cpp)
target_link_libraries(modbus_tcp PRIVATE logict_modbus_protocol)

logict_add_driver(modbus_rtu modbus_rtu.cpp)
target_link_libraries(modbus_rtu PRIVATE logict_modbus_protocol)

logict_add_driver(modbus_ascii modbus_ascii.cpp)
target_link_libraries(modbus_ascii PRIVATE logict_modbus_protocol)

logict_add_driver(modbus_server_serial modbus_server_serial.cpp)
target_link_libraries(modbus_server_serial PRIVATE logict_modbus_protocol)
