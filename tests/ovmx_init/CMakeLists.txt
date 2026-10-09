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

# test_console_policy: the executive-OPA0:-level vs PID-1-console-mute pair
# (rd vms-151). Compiles against the REAL src/kernel/ovmx_console_policy.h
# that both rinds use -- the executive's exec_console_printf() and PID 1's
# ovmx_boot_mute_kernel_console() -- so a drift in either constant fails here.
# Pure constants + one inline predicate: no syscall, no kernel, no /proc.
add_executable(test_console_policy test_console_policy.c)
target_include_directories(test_console_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/src/kernel
)
add_test(NAME console_policy_unit COMMAND test_console_policy)
set_tests_properties(console_policy_unit PROPERTIES
    LABELS "unit;ovmx_init;console"
    TIMEOUT 10
)

# test_sysgen_factory: rd vms-025 -- the SYSGEN parameter table is the SYSTEM's,
# not the parameter FILE's. Compiles against the REAL shared factory table
# (src/libvms/include/sysgen_factory.h) that SYSGEN.EXE and SYSBOOT both build
# on, reads the REAL shipped OVMXVMSSYS.PAR seed off disk, and derives the set of
# parameters it requires by scanning the shipping text of ovmx_init.c's cluster
# load. Pure: no /dev/vms, no boot, no RMS -- the header's readers are inline and
# this test never calls them.
add_executable(test_sysgen_factory test_sysgen_factory.c)
target_include_directories(test_sysgen_factory PRIVATE
    ${VMS_INCLUDE_DIR}
    ${VMS_FS_INCLUDE_DIR}
)
target_compile_definitions(test_sysgen_factory PRIVATE
    OVMX_REPO_ROOT="${CMAKE_SOURCE_DIR}")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_sysgen_factory PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME sysgen_factory_unit COMMAND test_sysgen_factory)
set_tests_properties(sysgen_factory_unit PROPERTIES
    LABELS "unit;ovmx_init;sysgen;cluster"
    TIMEOUT 10
)
