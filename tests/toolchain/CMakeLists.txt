# Toolchain round-trip tests (vms-ca9): LIBRARIAN.EXE + .OLB + LINK.EXE.
#
# Proves the object-library half of the OVMX-native toolchain: LIBRARIAN.EXE
# produces an .OLB (ar container) and LINK.EXE resolves an undefined symbol by
# selectively pulling the member that defines it. Uses the host-built toolchain
# binaries (LINK.EXE / LIBRARIAN.EXE / OVMXDUMP) + the system `ar` as an
# independent oracle.

add_test(
    NAME toolchain-olb-roundtrip
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_olb_roundtrip.sh
)
set_tests_properties(toolchain-olb-roundtrip PROPERTIES
    ENVIRONMENT "LINK_EXE=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LINK.EXE;LIBRARIAN_EXE=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LIBRARIAN.EXE;OVMXDUMP=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/OVMXDUMP;CC=${CMAKE_C_COMPILER}"
    TIMEOUT 120
    LABELS "toolchain;librarian;link"
)
