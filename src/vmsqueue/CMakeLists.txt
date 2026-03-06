# vmsqueue - VMS Queue Manager Library
#
# Provides batch and print queue management with persistent
# binary storage in QMAN$MASTER.DAT format.

add_library(vmsqueue ${OVMX_LIB_TYPE}
    vmsqueue.c
)

target_include_directories(vmsqueue PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VMS_INCLUDE_DIR}
)

target_link_libraries(vmsqueue PRIVATE vmsfs)

set_target_properties(vmsqueue PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSQUEUE\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmsqueue LIBRARY DESTINATION ${VMS_SHARE_DIR})
