# vmslnm - Logical Name Manager

set(VMSLNM_SOURCES
    lnm_table.c
    lnm_translate.c
    lnm_client.c
    lnm_defaults.c
)

add_library(vmslnm ${OVMX_LIB_TYPE} ${VMSLNM_SOURCES})

target_include_directories(vmslnm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VMS_INCLUDE_DIR}
)

# vmssys carries vms_kif_* -- the /dev/vms executive client. LNM$SYSTEM is
# executive-resident (vms-d37/#193), so lnm_create/_delete/_translate route it
# through vms_kif (vms-96e2). vmssys also exports the vms_kif.h include dir.
target_link_libraries(vmslnm PUBLIC pthread vmssys)

set_target_properties(vmslnm PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LIBVMSLNM\$SHR"
    SUFFIX ".EXE"
)
install(TARGETS vmslnm LIBRARY DESTINATION ${VMS_SHARE_DIR})

# Static twin for the static PID-1 bootstrap (STARTUP.EXE). IMGACT mode only.
if(OVMX_IMGACT)
    add_library(vmslnm_static STATIC ${VMSLNM_SOURCES})
    target_include_directories(vmslnm_static PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${VMS_INCLUDE_DIR}
    )
    target_link_libraries(vmslnm_static PUBLIC pthread vmssys)
endif()

# There is no logical name daemon. On OpenVMS the logical name tables live in
# the executive; there is no server process to start and no socket to connect
# to. VMSLNMD.EXE was an OVMX invention with zero clients product-wide, so it
# is gone rather than reduced (vms-a4b). The executive-resident tables that
# replace it are tracked by vms-ln0 / vms-d37.
