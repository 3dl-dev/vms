add_library(vmsfs ${OVMX_LIB_TYPE}
    vmsfs_translate.c
    vmsfs_version.c
    vmsfs_case.c
    vmsfs_protect.c
)
target_include_directories(vmsfs PUBLIC include ${VMS_INCLUDE_DIR} ${VMS_LNM_INCLUDE_DIR})
target_link_libraries(vmsfs PUBLIC vmslnm)
set_target_properties(vmsfs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSFS\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmsfs LIBRARY DESTINATION ${VMS_SHARE_DIR})
