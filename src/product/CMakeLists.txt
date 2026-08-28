# src/product/ - PRODUCT.EXE (PCSI-equivalent installer, bead vms-df9)
#
# External utility (SYS$SYSTEM:PRODUCT.EXE) -- deliberately not a DCL
# builtin, same pattern as src/install/ (INSTALL.EXE). Links vmsfs for
# vmsfs_protection_to_mode() (kit-metadata protection -> chmod(2) mode) and
# the shared ovmx_kit_reader library (built at top level, before this
# subdirectory is added) for kit parsing.

add_executable(vms_product
    product.c
)

target_include_directories(vms_product PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/product
    # vms-3a8: PRODUCT INSTALL routes through the executive Files-11 ACP and RMS
    # (see product.c header). vms_kif.h (ACP client, pulls ../kernel/vms_acp.h)
    # lives in libvmssys; rms_io.h + rms/rms.h (ACP-routed $CREATE/$PUT) in vmsrms.
    ${CMAKE_SOURCE_DIR}/src/libvmssys
    ${CMAKE_SOURCE_DIR}/src/vmsrms
    ${CMAKE_SOURCE_DIR}/src/vmsrms/include
)

target_compile_definitions(vms_product PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_product PRIVATE
    -Wall -Wextra
)

# vms-3a8: link vmsrms for the ACP-routed RMS write path (rms_open_named_handle /
# rms_io_* / rms_executive_absent); vmsrms PUBLIC-pulls vms + vmsfs + vmslnm and
# transitively vmsprocess + libvmssys (the vms_kif_acp_* /dev/vms client), so no
# separate vms/vmssys entry is needed. PRODUCT.EXE is a plain cc-linked hosted
# utility (NOT in the OVMX_LINK_NATIVE LINK.EXE graph), so this static-link change
# introduces no new cross-image symbol-vector registration.
target_link_libraries(vms_product PRIVATE vmsrms vmsfs ovmx_kit_reader)

set_target_properties(vms_product PROPERTIES
    OUTPUT_NAME "PRODUCT"
    SUFFIX ".EXE"
)
install(TARGETS vms_product RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
