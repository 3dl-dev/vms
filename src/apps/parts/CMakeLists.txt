# src/apps/parts — PARTS, the OVMX 0.2 RMS indexed-file demo application
# (beads vms-e97 / vms-f20). See docs/design-parts-demo.md.
#
# This builds a *host/dev* PARTS executable linked against the ordinary vmsrms
# library, so the RMS application logic (indexed create + keyed put/get) is
# exercised by ctest on every build. The VMS-NATIVE image (PARTS.EXE, built
# via cc -> LINK.EXE -> IMGACT and run under QEMU) is produced separately by
# src/apps/parts/mk_parts.sh and proven by tests/qemu/test_parts_rms_qemu.sh —
# that path is deliberately NOT a CMake target (it needs the musl link-native
# graph, exactly like DCL.EXE/LOGINOUT.EXE, which are built by build_link_native.sh,
# not by CMake).

add_executable(PARTS parts.c parts_db.c)
target_include_directories(PARTS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/vmsrms/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include)
target_link_libraries(PARTS PRIVATE vmsrms)

# Functional gate: create an indexed file, load records, prove keyed random
# lookups (first/middle/last found, a never-loaded key reported NOTFOUND).
add_test(
    NAME parts_rms_indexed_functional
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_parts_functional.sh
            $<TARGET_FILE:PARTS>
)
set_tests_properties(parts_rms_indexed_functional PROPERTIES
    LABELS "integration;rms;parts"
    TIMEOUT 60
)
