# src/apps/parts — PARTS, the OVMX 0.2 RMS indexed-file demo application
# (beads vms-e97 / vms-f20). See docs/design-parts-demo.md.
#
# This builds a PARTS executable linked against the ordinary vmsrms library,
# so the RMS application logic (indexed create + keyed put/get) is exercised
# by ctest on every build (host/dev use). On x86_64/aarch64 the SHIPPED
# PARTS.EXE is instead the VMS-NATIVE image (built via cc -> LINK.EXE ->
# IMGACT and run under QEMU) produced separately by src/apps/parts/mk_parts.sh
# and proven by tests/qemu/test_parts_rms_qemu.sh -- that path needs the musl
# link-native graph (Mode 2, host-arch only), exactly like DCL.EXE/LOGINOUT.EXE
# built by build_link_native.sh, and is deliberately NOT this CMake target.
#
# On vax (vms-64a, epic vms-509 unified cross-build Rung B) there is no Mode 2
# at all, so THIS target -- ordinary CMake, Decision-A ld.elf_so activation --
# IS vax's PARTS.EXE, the same relationship DCL.EXE/LOGINOUT.EXE already have
# to their own vax builds. Named + installed like every other shipped image so
# it can join the top-level `ovmx-images` aggregate.

add_executable(PARTS parts.c parts_db.c)
target_include_directories(PARTS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/vmsrms/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include)
target_link_libraries(PARTS PRIVATE vmsrms)
set_target_properties(PARTS PROPERTIES
    OUTPUT_NAME "PARTS"
    SUFFIX ".EXE"
)
if(DEFINED VMS_SYSTEM_DIR)
    install(TARGETS PARTS RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
endif()

if(BUILD_TESTS)
    # Functional gate: create an indexed file, load records, prove keyed
    # random lookups (first/middle/last found, a never-loaded key reported
    # NOTFOUND).
    add_test(
        NAME parts_rms_indexed_functional
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_parts_functional.sh
                $<TARGET_FILE:PARTS>
    )
    set_tests_properties(parts_rms_indexed_functional PROPERTIES
        LABELS "integration;rms;parts"
        TIMEOUT 60
    )
endif()
