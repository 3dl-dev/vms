# src/install/ - INSTALL utility (known image database, bead vms-913.5)
#
# External utility (SYS$SYSTEM:INSTALL.EXE) -- deliberately not a DCL
# builtin. The known file list is the executive's (VMS_IOCTL_KFE, rd vms-220);
# INSTALL keeps no database of its own.

add_executable(vms_install
    install.c
)

target_include_directories(vms_install PRIVATE
    ${VMS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/libvmssys
    ${CMAKE_SOURCE_DIR}/src/vmsprocess/include
)

target_compile_definitions(vms_install PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_install PRIVATE
    -Wall -Wextra
)

target_link_libraries(vms_install PRIVATE vms vmssys)  # vms_kif_kfe: the executive list (vms-220)

set_target_properties(vms_install PROPERTIES
    OUTPUT_NAME "INSTALL"
    SUFFIX ".EXE"
)
install(TARGETS vms_install RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

if(BUILD_TESTS)
    # The known file list is the executive's (rd vms-220): with no /dev/vms
    # (the host), every INSTALL request must fail honestly -- never report
    # success, never write a substitute on-disk database. The executive side
    # is proven against a real /dev/vms by tests/qemu/test_syssvc_kfe.c.
    add_test(NAME install_utility
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_install_test.sh
    )
    set_tests_properties(install_utility PROPERTIES
        LABELS "unit;install"
        TIMEOUT 30
        ENVIRONMENT "INSTALL_EXE=$<TARGET_FILE:vms_install>"
        # vms-ed4: stages a file under the real /vms/SYS0/SYSCOMMON tree.
        # Shares that SYSDISK with dcl-integration -> same lock so ctest
        # --parallel never runs them concurrently. Do not remove (Rule 8).
        RESOURCE_LOCK "sysdisk"
    )
endif()
