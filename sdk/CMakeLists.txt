add_library(logict_sdk SHARED
  src/types.cpp
  src/value.cpp
  src/block_planner.cpp
  src/codec.cpp
  src/tcp.cpp)
add_library(logict::sdk ALIAS logict_sdk)

target_include_directories(logict_sdk PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(logict_sdk
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE logict_warnings)
set_target_properties(logict_sdk PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

install(TARGETS logict_sdk LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
