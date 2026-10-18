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

if(LOGICT_WITH_MQTT)
  add_library(logict_mqtt STATIC
    mqtt/template.cpp
    mqtt/change_filter.cpp
    mqtt/agent_config.cpp
    mqtt/mqtt_agent.cpp)
  add_library(logict::mqtt ALIAS logict_mqtt)
  target_include_directories(logict_mqtt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(logict_mqtt
    PUBLIC logict::core
    PRIVATE eclipse-paho-mqtt-c::paho-mqtt3as-static logict_warnings)
  target_compile_definitions(logict_mqtt PUBLIC LOGICT_WITH_MQTT=1)
endif()
