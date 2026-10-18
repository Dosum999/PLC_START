find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(logict_core STATIC
  model/project.cpp
  tagdb/tagdb.cpp
  runtime/scaling.cpp
  runtime/plugin_loader.cpp
  runtime/channel_worker.cpp
  runtime/runtime.cpp
  util/log.cpp
  util/password.cpp)
add_library(logict::core ALIAS logict_core)

target_include_directories(logict_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logict_core
  PUBLIC logict::sdk spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads OpenSSL::Crypto ${CMAKE_DL_LIBS}
  PRIVATE logict_warnings)
