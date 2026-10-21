cmake_minimum_required(VERSION 3.28)

project(logict
  VERSION 0.1.0
  DESCRIPTION "Linux-native industrial connectivity server"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOGICT_BUILD_TESTS "Build unit tests" ON)
option(LOGICT_WITH_OPCUA "Build OPC UA server (open62541)" ON)
option(LOGICT_WITH_MQTT "Build MQTT agent (Paho C)" ON)
option(LOGICT_WITH_ODBC "Build the ODBC Client driver when an ODBC driver manager is found" ON)
option(LOGICT_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

include(GNUInstallDirs)
include(cmake/Dependencies.cmake)

# Common warning flags for first-party targets.
add_library(logict_warnings INTERFACE)
target_compile_options(logict_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
  $<$<BOOL:${LOGICT_WARNINGS_AS_ERRORS}>:-Werror>)

# Drivers are dropped next to the daemon in the build tree so it can be run in place.
set(LOGICT_DRIVER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/drivers)
set(LOGICT_INSTALL_DRIVER_DIR ${CMAKE_INSTALL_LIBDIR}/logict/drivers)

add_subdirectory(sdk)
add_subdirectory(core)
add_subdirectory(northbound)
add_subdirectory(drivers)
add_subdirectory(app/logictd)

if(LOGICT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
