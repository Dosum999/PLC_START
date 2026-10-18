# Builds a driver plugin (.so) into the shared driver directory.
function(logict_add_driver name)
  add_library(${name} MODULE ${ARGN})
  target_link_libraries(${name} PRIVATE logict::sdk logict_warnings)
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY ${LOGICT_DRIVER_OUTPUT_DIR}
    INSTALL_RPATH "$ORIGIN/../..")
  install(TARGETS ${name} LIBRARY DESTINATION ${LOGICT_INSTALL_DRIVER_DIR})
endfunction()

add_subdirectory(simulator)
add_subdirectory(modbus)
add_subdirectory(ls_xgt)
add_subdirectory(siemens)
