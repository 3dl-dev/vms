# tests/qemu/CMakeLists.txt
#
# Public sys$ system-service tests for the QEMU kernel-executive harness
# (vms-1d9, epic vms-6b8 Phase 0.5). Unlike tests/qemu/test_kmod_*.c (raw
# ioctl(2) against /dev/vms, compiled separately by tests/qemu/Dockerfile
# with plain gcc -static), these programs link the REAL src/libvms public
# sys$ API and are built through the normal OVMX CMake graph, so that
# reverting production code under src/libvms/syssvc/ is visible to them.
#
# Requires a real, insmod'd vms.ko at /dev/vms to do anything beyond prove
# buildability. When /dev/vms is absent (e.g. every dev/CI container build,
# which is NOT the OVMX runtime -- CLAUDE.md Rule 9), each test exits 77 and
# ctest reports SKIPPED, never a fake PASS.

function(qemu_syssvc_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        ${VMS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/src/libvmssys
    )
    target_link_libraries(${name} PRIVATE
        vms
        pthread
        m
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "integration;authenticity;executive"
        SKIP_RETURN_CODE 77
        TIMEOUT 60
    )
endfunction()

# sys$enq / sys$enqw / sys$deq (src/libvms/syssvc/sys_lock.c) against a
# real /dev/vms, exercised cross-process -- see the file header in
# test_syssvc_lock.c for the full scenario and why it catches a reverted
# sys_lock.c that raw ioctl tests (test_kmod_lock*.c) cannot.
qemu_syssvc_add_test(test_syssvc_lock
    ${CMAKE_CURRENT_SOURCE_DIR}/test_syssvc_lock.c)
