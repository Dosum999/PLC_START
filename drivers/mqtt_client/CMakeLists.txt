# Built only with MQTT support. Paho C is linked privately into the plugin
# (symbols not exported) so it cannot clash with the daemon's copy.
add_library(logict_mqtt_payload STATIC mqtt_payload.cpp)
target_include_directories(logict_mqtt_payload PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logict_mqtt_payload PUBLIC logict::sdk PRIVATE logict_warnings)
set_target_properties(logict_mqtt_payload PROPERTIES CXX_VISIBILITY_PRESET hidden)

logict_add_driver(mqtt_client mqtt_client.cpp)
target_link_libraries(mqtt_client PRIVATE logict_mqtt_payload eclipse-paho-mqtt-c::paho-mqtt3as-static)
target_link_options(mqtt_client PRIVATE -Wl,--exclude-libs,ALL)
