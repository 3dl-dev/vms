# src/install/ - INSTALL utility (known image database, bead vms-913.5)
#
# External utility (SYS$SYSTEM:INSTALL.EXE) — deliberately not a DCL
# builtin. Shares the on-disk KFE database format with the standalone
# lookup module in src/imgact/known_images.{c,h} (that module owns the
# format definition; INSTALL includes its header rather than duplicating
# the struct layout).

add_executable(vms_install
    install.c
)

target_include_directories(vms_install PRIVATE
    ${VMS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src/imgact
)

target_compile_definitions(vms_install PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE
)

target_compile_options(vms_install PRIVATE
    -Wall -Wextra
)

target_link_libraries(vms_install PRIVATE vms)

set_target_properties(vms_install PROPERTIES
    OUTPUT_NAME "INSTALL"
    SUFFIX ".EXE"
)
install(TARGETS vms_install RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

if(BUILD_TESTS)
    # Small helper used by test/run_install_test.sh to prove the DB file
    # INSTALL writes is loadable by the standalone known_images lookup
    # module via mmap (the IPC path IMGACT.EXE will eventually use).
    add_executable(test_lookup_integration test/test_lookup_integration.c)
    target_link_libraries(test_lookup_integration PRIVATE known_images)

    add_test(NAME install_utility
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run_install_test.sh
    )
    set_tests_properties(install_utility PROPERTIES
        LABELS "unit;install"
        TIMEOUT 30
        ENVIRONMENT "INSTALL_EXE=$<TARGET_FILE:vms_install>;LOOKUP_HELPER=$<TARGET_FILE:test_lookup_integration>"
        # vms-ed4: writes the real /vms/SYS0/SYSCOMMON/SYSEXE tree (VMS$KNOWN_
        # IMAGES.DAT). Shares that SYSDISK with dcl-integration -> same lock so
        # ctest --parallel never runs them concurrently. Do not remove (Rule 8).
        RESOURCE_LOCK "sysdisk"
    )
endif()
