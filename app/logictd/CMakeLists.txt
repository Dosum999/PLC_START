add_executable(logictd main.cpp)
target_link_libraries(logictd PRIVATE logict::core CLI11::CLI11 logict_warnings)
if(LOGICT_WITH_OPCUA)
  target_link_libraries(logictd PRIVATE logict::opcua)
  add_dependencies(logictd opcua_client)
endif()
if(LOGICT_WITH_PLCTAG)
  add_dependencies(logictd ab_controllogix ab_server)  # ab_server: libplctag's PLC simulator, used by tests
endif()
if(LOGICT_WITH_MQTT)
  target_link_libraries(logictd PRIVATE logict::mqtt)
endif()
set_target_properties(logictd PROPERTIES
  BUILD_RPATH "${CMAKE_BINARY_DIR}/lib"
  INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
# Bundled drivers are available when running from the build tree.
add_dependencies(logictd simulator modbus_tcp ls_xgt siemens_tcpip mitsubishi_ethernet modbus_rtu ls_xgt_cnet)
install(TARGETS logictd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
