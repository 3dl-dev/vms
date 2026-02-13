add_library(vmsprocess ${OVMX_LIB_TYPE}
    vms_pcb.c
    ast.c
    event_flags.c
    access_modes.c
)
target_include_directories(vmsprocess PUBLIC include ${VMS_INCLUDE_DIR})
target_link_libraries(vmsprocess PUBLIC pthread)
if(NOT OVMX_STATIC)
    set_target_properties(vmsprocess PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
endif()
install(TARGETS vmsprocess LIBRARY DESTINATION lib)
