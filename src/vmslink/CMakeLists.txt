# vmslink — LINK.EXE (the OVMX/VMS linker) + OVMXDUMP (image inspector).
# Bead vms-9dd, pillar vms-ade. These are host tools built by the bootstrap
# toolchain: they read ELF .o and write OVMX shareable images (ELF ET_DYN +
# .vms$sv symbol vector). They are not themselves VMS images.

add_executable(vmslink link.c)
target_include_directories(vmslink PRIVATE include)
set_target_properties(vmslink PROPERTIES OUTPUT_NAME "LINK" SUFFIX ".EXE")
install(TARGETS vmslink RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

add_executable(vmsdump dump_image.c)
target_include_directories(vmsdump PRIVATE include)
set_target_properties(vmsdump PROPERTIES OUTPUT_NAME "OVMXDUMP" SUFFIX "")
install(TARGETS vmsdump RUNTIME DESTINATION ${VMS_SYSTEM_DIR})

# DECC$SHR.EXE — the OVMX C run-time library shareable (bead vms-61f.1) — is NOT
# a host CMake target: it links the aarch64 musl libc.a + libgcc.a via LINK.EXE,
# so it only builds inside the arm64 musl container (this host is glibc). The
# build recipe is `mk_decc_shr.sh` (reusable by vms-61f.2 runtime init); the
# producer proof is `test/run_decc_shr.sh`, gated in CI by the `decc-shr` job.

# --- VMS-native LINK.EXE shareable-image graph (bead vms-b6a, pillar vms-ade) --
#
# Wires the mk_*_shr.sh / mk_dcl.sh / mk_loginout.sh recipes above (the
# vms-b65/c39 lib-migration chain, docs/design-link-native-toolchain.md) into
# `cmake --build`, in addition to (not replacing) each library's normal
# add_library(${OVMX_LIB_TYPE} ...) target above (src/libvms/CMakeLists.txt
# et al) — those keep serving host ctest unit tests, which link the library
# targets directly and know nothing about symbol vectors. This graph produces
# the SEPARATE artifact set (LIBVMSSYS$SHR.EXE, DECC$SHR.EXE, LIBVMSPROCESS$SHR.EXE,
# LIBVMSLNM$SHR.EXE, LIBVMSFS$SHR.EXE, LIBVMS$SHR.EXE, LIBVMSRMS$SHR.EXE, DCL.EXE,
# LOGINOUT.EXE) that IMGACT.EXE actually activates: ET_DYN images carrying a
# .vms$sv symbol vector and NO DT_NEEDED — never ld/ld.so.
#
# aarch64 OR x86_64 musl, detected from the compiler's target triple (the same
# precondition `run_dcl_native.sh`/`run_login_native.sh` document as "must run
# in the arm64/amd64 musl container"). x86_64 detection added by vms-6da,
# which extends this SAME target/detection mechanism (not a second custom
# target) once LINK.EXE's x86_64 backend (vms-bdf/vms-cb5f/vms-a66) landed --
# x86_64 is the project's primary arch (CLAUDE.md Rule 5).
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -dumpmachine
    OUTPUT_VARIABLE OVMX_CC_TARGET_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE OVMX_CC_DUMPMACHINE_RC
)
if(OVMX_CC_DUMPMACHINE_RC EQUAL 0 AND OVMX_CC_TARGET_TRIPLE MATCHES "musl" AND
   (OVMX_CC_TARGET_TRIPLE MATCHES "aarch64" OR OVMX_CC_TARGET_TRIPLE MATCHES "x86_64"))
    set(OVMX_LINK_NATIVE_SUPPORTED ON)
    if(OVMX_CC_TARGET_TRIPLE MATCHES "aarch64")
        set(OVMX_LINK_NATIVE_ARCH aarch64)
    else()
        set(OVMX_LINK_NATIVE_ARCH x86_64)
    endif()
else()
    set(OVMX_LINK_NATIVE_SUPPORTED OFF)
    set(OVMX_LINK_NATIVE_ARCH "")
endif()

option(OVMX_LINK_NATIVE
    "Build the VMS-native LINK.EXE shareable-image graph (LIBVMS\$SHR.EXE et al, DCL.EXE, LOGINOUT.EXE) -- aarch64 or x86_64 musl only"
    ${OVMX_LINK_NATIVE_SUPPORTED})

if(OVMX_LINK_NATIVE)
    if(NOT OVMX_LINK_NATIVE_SUPPORTED)
        # Fail honestly (CLAUDE.md Rule 9's spirit extended to the build): a
        # user who explicitly asked for this graph on a toolchain that cannot
        # produce it gets a build error, never a silent skip that reports
        # success without the artifacts.
        message(FATAL_ERROR
            "OVMX_LINK_NATIVE=ON but ${CMAKE_C_COMPILER} (-dumpmachine: "
            "'${OVMX_CC_TARGET_TRIPLE}') is not an aarch64 or x86_64 musl compiler. "
            "This graph is musl-only (docs/design-link-native-toolchain.md). Build "
            "inside an aarch64 or x86_64 musl toolchain (e.g. `docker run --platform "
            "linux/arm64 alpine:3.20` or a native x86_64 `alpine:3.20`, with "
            "`apk add cmake gcc musl-dev binutils linux-headers`) or unset OVMX_LINK_NATIVE.")
    endif()
    if(OVMX_STATIC)
        message(FATAL_ERROR "OVMX_LINK_NATIVE and OVMX_STATIC are mutually exclusive: "
            "LINK_NATIVE produces dynamically-activated VMS-native shareables, "
            "not statically-linked executables.")
    endif()

    set(OVMX_NATIVE_WORK ${CMAKE_BINARY_DIR}/link-native)
    set(OVMX_NATIVE_SYSLIB ${OVMX_NATIVE_WORK}/SYSLIB)
    set(OVMX_NATIVE_SYSEXE ${OVMX_NATIVE_WORK}/SYSEXE)
    set(OVMX_NATIVE_STAMP ${OVMX_NATIVE_WORK}/.built)

    # Reconfigure (not just rebuild) whenever a source/header the graph
    # compiles changes -- GLOB_RECURSE + CONFIGURE_DEPENDS re-globs at build
    # time so an added/removed TU is picked up without a manual re-run of cmake.
    file(GLOB_RECURSE OVMX_NATIVE_GRAPH_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/libvmssys/*.c
        ${CMAKE_SOURCE_DIR}/src/libvmssys/*.h
        ${CMAKE_SOURCE_DIR}/src/libvmssys/arch/${OVMX_LINK_NATIVE_ARCH}/*.S
        ${CMAKE_SOURCE_DIR}/src/vmsprocess/*.c
        ${CMAKE_SOURCE_DIR}/src/vmsprocess/include/*.h
        ${CMAKE_SOURCE_DIR}/src/vmslnm/*.c
        ${CMAKE_SOURCE_DIR}/src/vmslnm/include/*.h
        ${CMAKE_SOURCE_DIR}/src/vmsfs/*.c
        ${CMAKE_SOURCE_DIR}/src/vmsfs/include/*.h
        ${CMAKE_SOURCE_DIR}/src/libvms/*.c
        ${CMAKE_SOURCE_DIR}/src/libvms/include/*.h
        ${CMAKE_SOURCE_DIR}/src/vmsrms/*.c
        ${CMAKE_SOURCE_DIR}/src/vmsrms/include/*.h
        ${CMAKE_SOURCE_DIR}/src/vmsdcl/*.c
        ${CMAKE_SOURCE_DIR}/src/vmsdcl/include/*.h
        ${CMAKE_SOURCE_DIR}/src/vmsqueue/*.c
        ${CMAKE_SOURCE_DIR}/tools/vms_login.c
    )

    add_custom_command(
        OUTPUT ${OVMX_NATIVE_STAMP}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OVMX_NATIVE_SYSLIB} ${OVMX_NATIVE_SYSEXE}
        COMMAND ${CMAKE_COMMAND} -E env
                CC=${CMAKE_C_COMPILER}
                ARCH=${OVMX_LINK_NATIVE_ARCH}
                WORK=${OVMX_NATIVE_WORK}/work
                sh ${CMAKE_CURRENT_SOURCE_DIR}/build_link_native.sh
                    $<TARGET_FILE:vmslink> ${OVMX_NATIVE_SYSLIB} ${OVMX_NATIVE_SYSEXE} ${CMAKE_SOURCE_DIR}
        COMMAND ${CMAKE_COMMAND} -E touch ${OVMX_NATIVE_STAMP}
        DEPENDS
            vmslink
            ${CMAKE_CURRENT_SOURCE_DIR}/build_link_native.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_vmssys_shr.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_decc_shr.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_vmsprocess_shr.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_vmslnm_shr.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_vmsfs_shr.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_libvms_shr.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_vmsrms_shr.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_dcl.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/mk_loginout.sh
            ${OVMX_NATIVE_GRAPH_SOURCES}
        COMMENT "LINK.EXE: building the VMS-native shareable graph (5 libs + DECC\$SHR + DCL.EXE + LOGINOUT.EXE, ${OVMX_LINK_NATIVE_ARCH} musl, no ld/ld.so)"
        VERBATIM
    )
    add_custom_target(link_native_graph ALL DEPENDS ${OVMX_NATIVE_STAMP})
    message(STATUS "OVMX_LINK_NATIVE on (compiler target '${OVMX_CC_TARGET_TRIPLE}', ARCH=${OVMX_LINK_NATIVE_ARCH}) -- "
        "the VMS-native LINK.EXE shareable graph will be built into ${OVMX_NATIVE_WORK}")

    install(DIRECTORY ${OVMX_NATIVE_SYSLIB}/ DESTINATION ${VMS_SHARE_DIR})
    install(DIRECTORY ${OVMX_NATIVE_SYSEXE}/ DESTINATION ${VMS_SYSTEM_DIR})
else()
    message(STATUS "OVMX_LINK_NATIVE off (compiler target '${OVMX_CC_TARGET_TRIPLE}' is not "
        "aarch64-musl or x86_64-musl) -- the VMS-native LINK.EXE shareable graph is not built "
        "for this configuration. Build inside an aarch64 or x86_64 musl toolchain to produce it (see above).")
endif()
