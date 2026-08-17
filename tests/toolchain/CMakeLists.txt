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

# =============================================================================
# MMK.EXE — the vendored MadGoat MMK built as an OVMX image (self-host spine #4,
# bead vms-ec70).  The 15 vendored make-engine TUs (unmodified except for tagged
# "OVMX (vms-ec70)" seams) + the OVMX companions + the vms-486 PARSE_TABLES C
# grammar, linked against the OVMX RTL.  Proves MMK actually parses a descrip.mms
# through CLI$ compiled-CLD + real lib$table_parse + MMK's own parse_store, and
# emits the correct build plan.  See tests/corpus/tier3-mmk/ovmx/ for the
# clean-room (Rule 8) build shim, and run_mmk_parse.sh for the end-to-end proof.
#
# NB: this host ctest links MMK against the OVMX RTL libraries directly; the
# VMS-native LINK.EXE/IMGACT packaging of the SAME sources is the mk_mmk.sh
# recipe (src/vmslink/mk_mmk.sh), exercised on the container native-link path.
# =============================================================================
set(MMK_CORPUS  ${CMAKE_SOURCE_DIR}/tests/corpus/tier3-mmk)
set(MMK_OVMX    ${MMK_CORPUS}/ovmx)
set(MMK_COMPAT  ${MMK_OVMX}/ovmx_mmk_compat.h)

# Generate the embedded CLD source header from the vendored mmk_cld.cld.
set(MMK_CLD_SRC_H ${CMAKE_CURRENT_BINARY_DIR}/mmk_cld_src.h)
add_custom_command(
    OUTPUT ${MMK_CLD_SRC_H}
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/gen_mmk_cld_src.sh
            ${MMK_CORPUS}/mmk_cld.cld ${MMK_CLD_SRC_H}
    DEPENDS ${MMK_CORPUS}/mmk_cld.cld ${CMAKE_CURRENT_SOURCE_DIR}/gen_mmk_cld_src.sh
    COMMENT "Embedding mmk_cld.cld -> mmk_cld_src.h (vms-ec70)"
)

add_executable(mmk_native
    ${MMK_CORPUS}/mmk.c            ${MMK_CORPUS}/parse_descrip.c
    ${MMK_CORPUS}/parse_objects.c  ${MMK_CORPUS}/build_target.c
    ${MMK_CORPUS}/misc.c           ${MMK_CORPUS}/mem.c
    ${MMK_CORPUS}/fileio.c         ${MMK_CORPUS}/symbols.c
    ${MMK_CORPUS}/objects.c        ${MMK_CORPUS}/default_rules.c
    ${MMK_CORPUS}/get_rdt.c        ${MMK_CORPUS}/readdesc.c
    ${MMK_CORPUS}/str.c
    ${CMAKE_SOURCE_DIR}/tests/libvms/mmk_parse_tables.c
    ${MMK_OVMX}/ovmx_mmk_compat.c  ${MMK_OVMX}/ovmx_mmk_cld.c
    ${MMK_OVMX}/ovmx_mmk_sp.c      ${MMK_OVMX}/ovmx_mmk_cms.c
    ${MMK_OVMX}/ovmx_mmk_builtins.c
    ${MMK_CLD_SRC_H}
)
target_include_directories(mmk_native PRIVATE
    ${MMK_OVMX} ${MMK_CORPUS} ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/tests/libvms
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmsrms/include
    ${CMAKE_SOURCE_DIR}/src/vmsfs/include
    ${CMAKE_SOURCE_DIR}/src/vmslnm/include
    ${CMAKE_SOURCE_DIR}/src/vmsprocess/include
)
target_compile_definitions(mmk_native PRIVATE OVMX_MMK __CRTL_VER=80400000
    _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
target_compile_options(mmk_native PRIVATE
    -fno-builtin -fno-stack-protector -Wno-unused -Wno-implicit-fallthrough
    -include ${MMK_COMPAT})
# The vms-486 grammar is compiled in PRODUCTION mode here: its transitions fire
# MMK's real parse_store / parse_obj_store (see mmk_parse_tables.c).
set_source_files_properties(${CMAKE_SOURCE_DIR}/tests/libvms/mmk_parse_tables.c
    PROPERTIES COMPILE_DEFINITIONS OVMX_MMK_PRODUCTION)
target_link_libraries(mmk_native PRIVATE vms vmsrms vmsfs vmslnm vmsprocess vmssys pthread m)
set_target_properties(mmk_native PROPERTIES OUTPUT_NAME "MMK" SUFFIX ".EXE")

add_test(
    NAME toolchain-mmk-parse
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_mmk_parse.sh
)
set_tests_properties(toolchain-mmk-parse PROPERTIES
    ENVIRONMENT "MMK_EXE=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/MMK.EXE"
    TIMEOUT 120
    LABELS "toolchain;mmk;selfhost"
)

# spine #5 (vms-fe4): MMK plans a REAL multi-TU OVMX component (the freestanding
# runtime -- src/libvmssys vms_string/vms_snprintf/vms_math + a driver) described
# by tests/toolchain/component/OVMXRT.MMS: four TCC compiles, a LIBRARIAN archive
# and a LINK, in dependency order, byte-identical across two runs.  The parse+plan
# half on the host; execution rides QEMU (test_syssvc_mmk_build.c).
add_test(
    NAME toolchain-mmk-component-plan
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_mmk_component_plan.sh
)
set_tests_properties(toolchain-mmk-component-plan PROPERTIES
    ENVIRONMENT "MMK_EXE=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/MMK.EXE;REPO_SRC=${CMAKE_SOURCE_DIR}/src"
    TIMEOUT 120
    LABELS "toolchain;mmk;selfhost"
)

# spine #5 (vms-fe4): the OVMX-native LIBRARIAN.EXE + LINK.EXE build the real
# component's .OLB and image BYTE-IDENTICALLY across two independent builds --
# the "byte-identical twice" bar for the build output, proven on the real
# component objects (TCC's compile determinism is run_tcc_selfhost gen2==gen3).
add_test(
    NAME toolchain-mmk-component-build
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_mmk_component_build.sh
)
set_tests_properties(toolchain-mmk-component-build PROPERTIES
    ENVIRONMENT "LIBRARIAN_EXE=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LIBRARIAN.EXE;LINK_EXE=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/LINK.EXE;OVMXDUMP=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/OVMXDUMP;REPO_SRC=${CMAKE_SOURCE_DIR}/src;CC=${CMAKE_C_COMPILER}"
    TIMEOUT 120
    LABELS "toolchain;librarian;link;selfhost"
)

# vms-89d (self-host S4 LINK.EXE fixpoint, MMK path): MMK plans the multi-TU
# LINK.EXE self-host build described by tests/toolchain/component/LINKSH.MMS --
# two TCC compiles (link.c + ovmx_link_rms_io.c) then a six-shareable LINK, in
# dependency order, byte-identical across two runs.  The parse+plan half on the
# host; the real-toolchain gen2==gen3 OUTPUT fixpoint is proven by the dedicated
# CI job (ci.yml mmk-link-selfhost-fixpoint -> run_mmk_link_selfhost_build.sh),
# and MMK's mailbox-driven execution rides QEMU.  This is the LINK.EXE-fixpoint
# analog of toolchain-mmk-component-plan.
add_test(
    NAME toolchain-mmk-link-selfhost-plan
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_mmk_link_selfhost_plan.sh
)
set_tests_properties(toolchain-mmk-link-selfhost-plan PROPERTIES
    ENVIRONMENT "MMK_EXE=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/MMK.EXE;REPO_SRC=${CMAKE_SOURCE_DIR}/src"
    TIMEOUT 120
    LABELS "toolchain;mmk;selfhost;link"
)

# spine #6 (vms-d1b): the STATIC TCC.EXE prerequisite for the QEMU MMK-driven
# build.  mk_tcc_static.sh builds tinycc as the plain fork+execve compiler DCL
# activates as a foreign command (no IMGACT/shareable, matching the staged
# MMK.EXE/DCL.EXE), and this proves it compiles the REAL component runtime TUs
# (vms_string/vms_snprintf + the driver) to valid, BYTE-IDENTICAL objects -- the
# compile-step determinism the byte-identical-twice build bar needs.  The
# MMK-DRIVEN half (TCC run from MMK's persistent mailbox DCL against a real
# /dev/vms) rides QEMU (tests/qemu/test_syssvc_mmk_build.c).
add_test(
    NAME toolchain-tcc-static-component
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_tcc_static_component.sh
)
set_tests_properties(toolchain-tcc-static-component PROPERTIES
    ENVIRONMENT "TCC_SRC=${CMAKE_SOURCE_DIR}/third-party/tcc/src;REPO_SRC=${CMAKE_SOURCE_DIR}/src;COMPONENT=${CMAKE_CURRENT_SOURCE_DIR}/component;CC=${CMAKE_C_COMPILER}"
    TIMEOUT 300
    LABELS "toolchain;tcc;selfhost"
)
