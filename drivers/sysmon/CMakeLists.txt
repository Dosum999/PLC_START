# Parsers shared by the driver and unit tests.
add_library(logict_sysmon STATIC sysmon.cpp)
target_include_directories(logict_sysmon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logict_sysmon PRIVATE logict_warnings)
set_target_properties(logict_sysmon PROPERTIES CXX_VISIBILITY_PRESET hidden)

logict_add_driver(system_monitor system_monitor.cpp)
target_link_libraries(system_monitor PRIVATE logict_sysmon)
