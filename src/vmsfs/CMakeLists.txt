set(VMSFS_SOURCES
    vmsfs_translate.c
    vmsfs_version.c
    vmsfs_case.c
    vmsfs_protect.c
    vmsfs_device.c
)
add_library(vmsfs ${OVMX_LIB_TYPE} ${VMSFS_SOURCES})
target_include_directories(vmsfs PUBLIC include ${VMS_INCLUDE_DIR} ${VMS_LNM_INCLUDE_DIR})
target_link_libraries(vmsfs PUBLIC vmslnm)
set_target_properties(vmsfs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSFS\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmsfs LIBRARY DESTINATION ${VMS_SHARE_DIR})

# Static twin for the static PID-1 bootstrap (STARTUP.EXE). IMGACT mode only.
if(OVMX_IMGACT)
    add_library(vmsfs_static STATIC ${VMSFS_SOURCES})
    target_include_directories(vmsfs_static PUBLIC include ${VMS_INCLUDE_DIR} ${VMS_LNM_INCLUDE_DIR})
    target_link_libraries(vmsfs_static PUBLIC vmslnm_static)
endif()
