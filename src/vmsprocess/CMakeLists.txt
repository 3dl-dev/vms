set(VMSPROCESS_SOURCES
    vms_pcb.c
    ast.c
    event_flags.c
    access_modes.c
)
add_library(vmsprocess ${OVMX_LIB_TYPE} ${VMSPROCESS_SOURCES})
target_include_directories(vmsprocess PUBLIC include ${VMS_INCLUDE_DIR})
target_link_libraries(vmsprocess PUBLIC pthread)
set_target_properties(vmsprocess PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSPROCESS\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmsprocess LIBRARY DESTINATION ${VMS_SHARE_DIR})

# Static twin for the static PID-1 bootstrap (STARTUP.EXE), which cannot link
# a shareable (.EXE) image. Built only in OVMX_IMGACT (dynamic) mode.
if(OVMX_IMGACT)
    add_library(vmsprocess_static STATIC ${VMSPROCESS_SOURCES})
    target_include_directories(vmsprocess_static PUBLIC include ${VMS_INCLUDE_DIR})
    target_link_libraries(vmsprocess_static PUBLIC pthread)
endif()
