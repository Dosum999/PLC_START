# Third-party dependencies, pinned. System packages are used when available
# (FIND_PACKAGE_ARGS), otherwise sources are fetched.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  FIND_PACKAGE_ARGS 3.11)

FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS 1.12)
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)

FetchContent_Declare(CLI11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
  GIT_TAG v2.4.2
  GIT_SHALLOW TRUE
  FIND_PACKAGE_ARGS 2.3)

FetchContent_MakeAvailable(nlohmann_json spdlog CLI11)

if(LOGICT_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
    FIND_PACKAGE_ARGS NAMES GTest)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

if(LOGICT_WITH_OPCUA)
  FetchContent_Declare(open62541
    GIT_REPOSITORY https://github.com/open62541/open62541.git
    GIT_TAG v1.5.9
    GIT_SHALLOW TRUE)
  set(UA_ENABLE_ENCRYPTION OPENSSL CACHE STRING "" FORCE)
  set(UA_MULTITHREADING 100 CACHE STRING "" FORCE)
  set(UA_NAMESPACE_ZERO REDUCED CACHE STRING "" FORCE)
  set(UA_LOGLEVEL 200 CACHE STRING "" FORCE)
  set(UA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(UA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(UA_ENABLE_PUBSUB OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(open62541)
endif()

if(LOGICT_WITH_MQTT)
  FetchContent_Declare(paho_mqtt_c
    GIT_REPOSITORY https://github.com/eclipse-paho/paho.mqtt.c.git
    GIT_TAG v1.3.16
    GIT_SHALLOW TRUE)
  set(PAHO_WITH_SSL ON CACHE BOOL "" FORCE)
  set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(PAHO_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(PAHO_ENABLE_CPACK OFF CACHE BOOL "" FORCE)
  set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  set(PAHO_HIGH_PERFORMANCE ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(paho_mqtt_c)
endif()

option(LOGICT_WITH_PLCTAG "Build the Rockwell driver (libplctag)" ON)
if(LOGICT_WITH_PLCTAG)
  FetchContent_Declare(libplctag
    GIT_REPOSITORY https://github.com/libplctag/libplctag.git
    GIT_TAG v2.7.2
    GIT_SHALLOW TRUE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(USE_MEM_SANITIZERS OFF CACHE BOOL "" FORCE)
  set(USE_SERVER_MEM_SANITIZERS OFF CACHE BOOL "" FORCE)
  # libplctag links its tools with -static, which cannot be combined with our
  # sanitizer flags; USE_VALGRIND only switches that static linking off.
  if(CMAKE_C_FLAGS MATCHES "-fsanitize" OR CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    set(USE_VALGRIND ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_MakeAvailable(libplctag)
endif()

if(LOGICT_WITH_ODBC)
  find_package(ODBC)
  if(NOT ODBC_FOUND)
    message(STATUS "ODBC driver manager not found (install unixodbc-dev); ODBC Client driver disabled")
    set(LOGICT_WITH_ODBC OFF)
  endif()
endif()
