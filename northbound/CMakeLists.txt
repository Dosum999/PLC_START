if(LOGICT_WITH_OPCUA)
  add_library(logict_opcua STATIC
    opcua/variant.cpp
    opcua/opcua_server.cpp)
  add_library(logict::opcua ALIAS logict_opcua)
  target_include_directories(logict_opcua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(logict_opcua
    PUBLIC logict::core open62541::open62541
    PRIVATE logict_warnings)
  target_compile_definitions(logict_opcua PUBLIC LOGICT_WITH_OPCUA=1)
endif()
