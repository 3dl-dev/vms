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
)

target_compile_definitions(vms_product PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_product PRIVATE
    -Wall -Wextra
)

target_link_libraries(vms_product PRIVATE vmsfs ovmx_kit_reader)

set_target_properties(vms_product PROPERTIES
    OUTPUT_NAME "PRODUCT"
    SUFFIX ".EXE"
)
install(TARGETS vms_product RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
