# vmsrms - Record Management Services
#
# Provides the VMS-compatible RMS API for file and record operations.
# Supports sequential, relative, and indexed file organizations.

add_library(vmsrms ${OVMX_LIB_TYPE}
    rms_core.c
    rms_seq.c
    rms_rel.c
    rms_idx.c
    rms_record.c
    rms_parse.c
    rms_search.c
    rms_util.c
)

target_include_directories(vmsrms PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
)

target_link_libraries(vmsrms PUBLIC vms vmsfs vmslnm)

set_target_properties(vmsrms PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSRMS\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmsrms LIBRARY DESTINATION ${VMS_SHARE_DIR})
install(DIRECTORY include/rms/ DESTINATION include/vms/rms)
