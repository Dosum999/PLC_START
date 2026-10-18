# libplctag (MPL-2.0) is linked statically and kept private to the plugin.
logict_add_driver(ab_controllogix allen_bradley.cpp)
target_include_directories(ab_controllogix PRIVATE ${libplctag_SOURCE_DIR}/src/libplctag/lib)
target_link_libraries(ab_controllogix PRIVATE plctag_static)
target_link_options(ab_controllogix PRIVATE -Wl,--exclude-libs,ALL)

logict_add_driver(omron_nj omron_nj.cpp)
target_include_directories(omron_nj PRIVATE ${libplctag_SOURCE_DIR}/src/libplctag/lib)
target_link_libraries(omron_nj PRIVATE plctag_static)
target_link_options(omron_nj PRIVATE -Wl,--exclude-libs,ALL)
