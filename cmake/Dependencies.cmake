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
