add_library(vmsprocess ${OVMX_LIB_TYPE}
    vms_pcb.c
    ast.c
    event_flags.c
    access_modes.c
)
target_include_directories(vmsprocess PUBLIC include ${VMS_INCLUDE_DIR})
target_link_libraries(vmsprocess PUBLIC pthread)
set_target_properties(vmsprocess PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSPROCESS\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmsprocess LIBRARY DESTINATION ${VMS_SHARE_DIR})
