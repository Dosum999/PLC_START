include(GoogleTest)

add_executable(logict_unit_tests
  unit/value_test.cpp
  unit/codec_test.cpp
  unit/block_planner_test.cpp
  unit/scaling_test.cpp
  unit/model_test.cpp
  unit/runtime_test.cpp
  unit/modbus_protocol_test.cpp
  unit/modbus_tcp_test.cpp
  unit/ls_xgt_protocol_test.cpp
  unit/ls_xgt_test.cpp
  unit/s7_protocol_test.cpp
  unit/mc_protocol_test.cpp
  unit/cnet_protocol_test.cpp
  support/modbus_test_server.cpp
  support/xgt_test_server.cpp)
if(LOGICT_WITH_MQTT)
  target_sources(logict_unit_tests PRIVATE unit/mqtt_test.cpp)
  target_link_libraries(logict_unit_tests PRIVATE logict::mqtt)
endif()
target_include_directories(logict_unit_tests PRIVATE support)
target_link_libraries(logict_unit_tests PRIVATE logict::core logict_modbus_protocol logict_xgt_protocol logict_s7_protocol logict_mc_protocol GTest::gtest_main logict_warnings)
target_compile_definitions(logict_unit_tests PRIVATE
  LOGICT_TEST_DRIVER_DIR="${LOGICT_DRIVER_OUTPUT_DIR}"
  LOGICT_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(logict_unit_tests PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
add_dependencies(logict_unit_tests simulator modbus_tcp ls_xgt)

# GCC 13 ASan/TSan can hang at startup on kernels with high mmap ASLR entropy
# (Linux >= 6.5, incl. WSL2). Run sanitizer builds with ASLR disabled.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize")
  find_program(SETARCH setarch)
  if(SETARCH)
    set_target_properties(logict_unit_tests PROPERTIES CROSSCOMPILING_EMULATOR "${SETARCH};-R")
  endif()
endif()
gtest_discover_tests(logict_unit_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 60)
