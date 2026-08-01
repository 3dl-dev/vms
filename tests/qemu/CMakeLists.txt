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
#
# HONESTY NOTE on what this ctest registration actually buys: in every
# environment where `ctest` itself runs (dev machine, the regular CI build
# job -- none of which has /dev/vms, since Docker/podman is not the OVMX
# runtime), this add_test() entry SKIPs (exit 77) -- it never PASSes
# anywhere ctest runs. Inside the one environment where /dev/vms IS real
# (the QEMU kernel-executive job), the binary is invoked directly by
# tests/qemu/init.sh (see the test_kmod_*/test_syssvc_* loop there), NOT
# through ctest -- ctest is not present in that initramfs at all. So this
# ctest entry never executes as a PASSing assertion anywhere: it reports
# Skipped everywhere it can run, and where it would matter it isn't the one
# running. What it buys is build-graph inclusion (qemu_syssvc_tests below
# fails to configure/build if the source doesn't compile) and an honest,
# visible SKIP rather than a silent gap in `ctest` output -- it is NOT
# coverage, despite appearing in the ctest listing next to tests that are.
#
# GENERALITY (vms-1d9 round 3): every test_syssvc_*.c source under this
# directory is discovered by glob and registered automatically -- there is
# no per-file list to remember to update. This is the fix for a real defect
# found by adversarial review of round 2: the harness build/initramfs steps
# named exactly ONE binary (test_syssvc_lock) by literal string in
# tests/qemu/Dockerfile, so the next test_syssvc_*.c a future item adds
# would build and SKIP fine locally, then silently never run in QEMU CI.
# tests/qemu/Dockerfile builds the qemu_syssvc_tests target below (not any
# one test by name) and copies build-static/bin/test_syssvc_* by glob.

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
    set_property(GLOBAL APPEND PROPERTY QEMU_SYSSVC_TEST_TARGETS ${name})
endfunction()

# Discover every test_syssvc_*.c in this directory and register it. Note:
# CMake's file(GLOB) does not re-run automatically when a file is ADDED
# (only when an existing tracked file changes) -- a fresh `cmake -B` (which
# every clean build, including tests/qemu/Dockerfile's, always does) picks
# up new files. This is a known CMake glob limitation, not a gap specific
# to this harness: an in-place `cmake --build` in a build dir that already
# exists at config time would miss a newly-added file until reconfigured.
file(GLOB QEMU_SYSSVC_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_syssvc_*.c)
foreach(_src ${QEMU_SYSSVC_SOURCES})
    get_filename_component(_name ${_src} NAME_WE)
    qemu_syssvc_add_test(${_name} ${_src})
endforeach()

# Single build target covering every registered test_syssvc_* binary, so
# callers (tests/qemu/Dockerfile) never need to name a specific test.
get_property(_qemu_syssvc_test_targets GLOBAL PROPERTY QEMU_SYSSVC_TEST_TARGETS)
add_custom_target(qemu_syssvc_tests DEPENDS ${_qemu_syssvc_test_targets})
