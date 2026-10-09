# tests/netbsd/guest/CMakeLists.txt - vax facility guest-tool builds via the
# unified CMake graph (rd vms-74b, epic vms-509 "unified cross-platform
# build" Rung D; docs/design-unified-cross-build.md §5/§8).
#
# Migrates the four NetBSD/vax facility guest TEST TOOLS off their
# hand-written tools/cross-vax/build-{eflag,access,proctab,mbx}-tool-vax.sh
# scripts onto ordinary CMake add_executable targets, built through the SAME
# top-level project + toolchain-file path as ovmx-images (rd vms-64a, Rung B)
# instead of a second, hand-maintained TU/include/link-line list. These are
# NOT shipped images (they never join the `ovmx-images` aggregate) -- they
# are the fast per-PR width-safety complement to the nightly SIMH runtime
# proofs (tests/lab-vax/run-{eflag,access,proctab,mbx}.sh): ordinary
# userspace programs that reach /dev/vms through the NetBSD kif_transport
# leaf (src/libvmssys/kif_transport_netbsd.c) and prove each shared wire
# contract (src/kernel-netbsd/vms_{eflag,access,ast,proctab,mbx}_nb.h) is
# genuinely ILP32-safe -- vax is the one target where an LP64-ism in those
# headers compiles clean everywhere else and only breaks here.
#
# Only meaningful on the NetBSD substrate (kif_transport_netbsd.c does not
# build for Linux at all -- see its own file header); this whole subdirectory
# is a no-op unless CMAKE_SYSTEM_NAME is NetBSD, so it is added unconditionally
# from the top-level CMakeLists.txt (independent of BUILD_TESTS, which
# defaults OFF when cross-compiling -- these targets are not host-executed
# ctest binaries, they are build-only cross-compile + link proofs, same as
# every ovmx-images member).
#
# Statically linked (-static), UNLIKE the Decision-A shipped images
# (ovmx-images is deliberately dynamic, ld.elf_so-activated) -- this mirrors
# what every build-*-tool-vax.sh script already did by hand; these are
# harness tools copied onto a SIMH disk image, not installed VMS images, so
# Decision-A's activation contract does not apply to them.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    return()
endif()

set(_OVMX_NB_GUEST_LIBVMSSYS_DIR ${CMAKE_SOURCE_DIR}/src/libvmssys)
set(_OVMX_NB_GUEST_KMOD_DIR ${CMAKE_SOURCE_DIR}/src/kernel-netbsd)
set(_OVMX_NB_GUEST_KIF_TRANSPORT ${_OVMX_NB_GUEST_LIBVMSSYS_DIR}/kif_transport_netbsd.c)

# One aggregate target = the single authoritative "which facility guest
# tools cross-build for vax" list -- the CMake-native replacement for the
# four separate build-*-tool-vax.sh invocations. Porting/adding a guest tool
# = adding its add_executable here, never a new script (the same
# operator-freeze ovmx-images already follows).
add_custom_target(ovmx-netbsd-guest-tools)

function(ovmx_netbsd_guest_tool _name _src)
    add_executable(${_name} ${_src} ${_OVMX_NB_GUEST_KIF_TRANSPORT})
    target_include_directories(${_name} PRIVATE
        ${_OVMX_NB_GUEST_LIBVMSSYS_DIR}
        ${_OVMX_NB_GUEST_KMOD_DIR}
    )
    target_compile_options(${_name} PRIVATE -O -Wall -Wextra)
    # -static: matches every build-*-tool-vax.sh's hand link line (see file
    # header above) -- these are harness tools, not Decision-A images.
    target_link_options(${_name} PRIVATE -static)
    add_dependencies(ovmx-netbsd-guest-tools ${_name})
endfunction()

ovmx_netbsd_guest_tool(vmseflag   ${CMAKE_CURRENT_SOURCE_DIR}/vmseflag.c)
ovmx_netbsd_guest_tool(vmsaccess  ${CMAKE_CURRENT_SOURCE_DIR}/vmsaccess.c)
ovmx_netbsd_guest_tool(vmsproctab ${CMAKE_CURRENT_SOURCE_DIR}/vmsproctab.c)
ovmx_netbsd_guest_tool(vmsmbx     ${CMAKE_CURRENT_SOURCE_DIR}/vmsmbx.c)

# ---------------------------------------------------------------------------
# The $STATUS proof images (rd vms-b869, parent vms-3b3f) -- STSNORM.EXE and
# STSCOND.EXE, one source (status_exit.c) built twice. UNLIKE the static harness
# tools above these are ordinary Decision-A OVMX/VAX images (dynamic, ld.elf_so,
# statically bound to the OVMX RTL -- the same link shape as AUTHORIZE.EXE), so
# a DCL RUN of them goes through exactly the activation path any shipped VAX
# image does. NOT shipped: they never join `ovmx-images'. Built by
# `cmake --build --target ovmx-vax-status-images' (tools/cross-vax/
# build-ovmx-images-vax-cmake.sh per PR; tests/lab-vax/run-boot.sh status-gate
# stages them into SYS$SYSTEM and RUNs them under SIMH).
# ---------------------------------------------------------------------------
add_custom_target(ovmx-vax-status-images)

function(ovmx_vax_status_image _target _name _cond)
    add_executable(${_target} ${CMAKE_CURRENT_SOURCE_DIR}/status_exit.c)
    target_include_directories(${_target} PRIVATE ${VMS_INCLUDE_DIR})
    target_compile_definitions(${_target} PRIVATE
        OVMX_STATUS_NAME="${_name}"
        OVMX_STATUS_COND=${_cond}
    )
    target_compile_options(${_target} PRIVATE -O -Wall -Wextra)
    target_link_libraries(${_target} PRIVATE
        vms vmsrms vmsfs vmslnm vmsprocess vmssys)
    set_target_properties(${_target} PROPERTIES
        OUTPUT_NAME "${_name}"
        SUFFIX ".EXE"
    )
    add_dependencies(ovmx-vax-status-images ${_target})
endfunction()

ovmx_vax_status_image(vax_status_stsnorm STSNORM 0x00000001)
ovmx_vax_status_image(vax_status_stscond STSCOND 0x0FEDC0A9)

# ---------------------------------------------------------------------------
# Width-safety teeth (negctl), re-expressed as ctest build-negatives.
#
# enable_testing() is called here UNCONDITIONALLY (not gated on the
# top-level BUILD_TESTS, which defaults OFF under CMAKE_CROSSCOMPILING) so
# these four checks are always registered and always run whenever this
# subdirectory is configured -- an absent/skipped teeth check is a failing
# teeth check (CLAUDE.md Rule 7). Idempotent if the top level already called
# it.
# ---------------------------------------------------------------------------
enable_testing()

set(_OVMX_NB_GUEST_FACILITIES eflag access proctab mbx)
foreach(_facility ${_OVMX_NB_GUEST_FACILITIES})
    add_test(
        NAME ${_facility}_width_negctl_vax
        COMMAND
            ${CMAKE_CURRENT_SOURCE_DIR}/negctl/run_negctl.sh
            "${CMAKE_C_COMPILER}"
            "${CMAKE_SYSROOT}"
            "${_facility}"
            "${CMAKE_CURRENT_SOURCE_DIR}/negctl/${_facility}_negctl.c"
            "${CMAKE_CURRENT_BINARY_DIR}/negctl/${_facility}"
    )
    set_tests_properties(${_facility}_width_negctl_vax PROPERTIES
        LABELS "negctl;vax;netbsd;facility-guest-tools"
        TIMEOUT 60
    )
endforeach()

# ---------------------------------------------------------------------------
# vms-08cb, epic vms-509 "unified cross-platform build" Rung E
# (docs/design-unified-cross-build.md §5/§6/§8): two more negctl teeth
# migrated here from the per-image CI gates the ovmx-images aggregate
# (vax-cmake-images) now supersedes -- same run_negctl.sh mechanism as the
# four facility teeth above, labeled separately (LABELS "vax-images") so the
# vax-cmake-images CI job can select exactly these two without re-running the
# facility-guest-tools set (that stays netbsd-vax-facility-tools-crosscompile's
# job, unchanged by this rung).
#   * libvmssys_ilp32_negctl_vax  -- was build-libvmssys-vax.sh's
#     CROSSCOMPILE_NEGCTL=1 branch (the toolchain-level ILP32 width gate).
#     Named "*_ilp32_negctl_vax", NOT "*_width_negctl_vax" like the four
#     facility tests above: build-facility-tools-vax-cmake.sh selects its set
#     with `ctest -R '_width_negctl_vax$'` and asserts an exact count of 4 --
#     a "*_width_negctl_vax" name here would silently join that job's set and
#     break its count assertion.
#   * librarian_negctl_vax        -- was build-librarian-vax.sh's
#     CROSSCOMPILE_NEGCTL=1 branch (a deliberately-broken TU must reject).
# ---------------------------------------------------------------------------
add_test(
    NAME libvmssys_ilp32_negctl_vax
    COMMAND
        ${CMAKE_CURRENT_SOURCE_DIR}/negctl/run_negctl.sh
        "${CMAKE_C_COMPILER}"
        "${CMAKE_SYSROOT}"
        "libvmssys"
        "${CMAKE_CURRENT_SOURCE_DIR}/negctl/libvmssys_ilp32_negctl.c"
        "${CMAKE_CURRENT_BINARY_DIR}/negctl/libvmssys"
)
set_tests_properties(libvmssys_ilp32_negctl_vax PROPERTIES
    LABELS "negctl;vax;netbsd;vax-images"
    TIMEOUT 60
)

add_test(
    NAME librarian_negctl_vax
    COMMAND
        ${CMAKE_CURRENT_SOURCE_DIR}/negctl/run_negctl.sh
        "${CMAKE_C_COMPILER}"
        "${CMAKE_SYSROOT}"
        "librarian"
        "${CMAKE_CURRENT_SOURCE_DIR}/negctl/librarian_negctl.c"
        "${CMAKE_CURRENT_BINARY_DIR}/negctl/librarian"
)
set_tests_properties(librarian_negctl_vax PROPERTIES
    LABELS "negctl;vax;netbsd;vax-images"
    TIMEOUT 60
)
