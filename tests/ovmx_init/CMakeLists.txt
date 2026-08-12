# tests/ovmx_init/CMakeLists.txt
#
# test_opcom_kmsg: unit tests for the /dev/kmsg -> operator-console/
# OPERATOR.LOG reformatter (rd vms-32a). Compiles the REAL
# src/ovmx_init/opcom_kmsg.c, not a copy (CLAUDE.md Rule 10) --
# opcom_kmsg_classify() itself is a pure function, no /dev/kmsg or kernel
# needed, so this runs everywhere ctest runs. The real-kernel end-to-end
# proof is tests/qemu/test_job_control_console.sh (see that file's OPCOM
# routing section). Include dirs and link libs mirror src/ovmx_init/
# CMakeLists.txt: opcom_kmsg.c resolves SYS$MANAGER:OPERATOR.LOG through
# vmsfs, the same accessor sys_operator.c's sys$sndopr uses.
add_executable(test_opcom_kmsg
    test_opcom_kmsg.c
    ${CMAKE_SOURCE_DIR}/src/ovmx_init/opcom_kmsg.c
)
target_include_directories(test_opcom_kmsg PRIVATE
    ${CMAKE_SOURCE_DIR}/src/ovmx_init
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
    ${VMS_LNM_INCLUDE_DIR}
    ${VMS_PROCESS_INCLUDE_DIR}
)
target_link_libraries(test_opcom_kmsg PRIVATE vmsprocess vmsfs vmslnm vmssys pthread)

add_test(NAME opcom_kmsg_unit COMMAND test_opcom_kmsg)
set_tests_properties(opcom_kmsg_unit PROPERTIES
    LABELS "unit;ovmx_init;opcom"
    TIMEOUT 10
)
