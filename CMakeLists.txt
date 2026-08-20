cmake_minimum_required(VERSION 3.16)
project(OVMX
    VERSION 0.1.0
    DESCRIPTION "OpenVMS-Compatible Linux Distribution"
    LANGUAGES C ASM
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Always generate compile_commands.json (used by clang-tidy static analysis)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Expose POSIX and GNU APIs (sigaction, timer_t, FNM_CASEFOLD, etc.)
add_compile_definitions(_GNU_SOURCE)

# Common compile options
add_compile_options(-Wall -Wextra)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# VMS directory layout
set(VMS_SHARE_DIR "vms/SYS0/SYSCOMMON/SYSLIB")
set(VMS_SYSTEM_DIR "vms/SYS0/SYSCOMMON/SYSEXE")

# Runtime library search path — all binaries find shareable images in SYS$SHARE
set(CMAKE_INSTALL_RPATH "/vms/SYS0/SYSCOMMON/SYSLIB")
set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
set(CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")

# Global VMS include path (for #include <descrip.h> etc.)
set(VMS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/libvms/include)
set(VMS_RMS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmsrms/include)
set(VMS_FS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmsfs/include)
set(VMS_LNM_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmslnm/include)
set(VMS_PROCESS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/src/vmsprocess/include)

# Options
#
# Unified cross-build (vms-509): under a cross toolchain file (CMAKE_
# CROSSCOMPILING), default the un-runnable / host-only knobs off, mirroring
# docs/design-unified-cross-build.md §3-A. Cross tests cannot execute on the
# build host, so BUILD_TESTS defaults OFF when cross-compiling; a caller can
# still force it back on with -DBUILD_TESTS=ON. BUILD_TOOLS stays ON in both
# cases -- the shipped boot images live behind it.
if(CMAKE_CROSSCOMPILING)
    set(_OVMX_BUILD_TESTS_DEFAULT OFF)
else()
    set(_OVMX_BUILD_TESTS_DEFAULT ON)
endif()
option(BUILD_TESTS "Build test programs" ${_OVMX_BUILD_TESTS_DEFAULT})
option(BUILD_TOOLS "Build VMS tools" ON)
option(BUILD_FUSE "Build FUSE ODS-2 driver (requires libfuse)" OFF)
option(OVMX_STATIC "Build all libraries as static and link statically" OFF)
option(OVMX_IMGACT "Build shareable images + PT_INTERP=IMGACT.EXE (musl, QEMU only)" OFF)

# IMGACT.EXE, the OVMX image activator, is the ELF interpreter for dynamically
# activated OVMX executables. Its path on the target system disk (SYS$SYSTEM).
set(OVMX_IMGACT_PATH "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE")

if(OVMX_STATIC AND OVMX_IMGACT)
    message(FATAL_ERROR "OVMX_STATIC and OVMX_IMGACT are mutually exclusive: "
        "IMGACT mode produces dynamically activated (shared) images.")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    # Decision-A linkage (rd vms-42d, docs/design-unified-cross-build.md §2.1):
    # a THIRD linkage mode, distinct from both branches below. The OVMX RTL
    # libraries build as STATIC archives, but images stay ORDINARY DYNAMIC
    # NetBSD ELF32 executables activated by /usr/libexec/ld.elf_so -- this is
    # NOT OVMX_STATIC (which adds a global -static, producing an executable
    # with no dynamic linker at all) and NOT OVMX_IMGACT (whose PT_INTERP=
    # IMGACT.EXE activation has no meaning on this substrate; ld.elf_so is the
    # activator here). Unconditional for the NetBSD substrate -- there is no
    # SHARED mode on netbsd-vax today (docs/design-ovmx-netbsd-syskrnl.md).
    if(OVMX_STATIC OR OVMX_IMGACT)
        message(FATAL_ERROR
            "OVMX_STATIC and OVMX_IMGACT are host-substrate (Linux) link "
            "modes; the NetBSD substrate uses Decision-A linkage "
            "unconditionally (rd vms-42d) and must not combine with either.")
    endif()
    set(OVMX_LIB_TYPE STATIC)
elseif(OVMX_STATIC)
    set(OVMX_LIB_TYPE STATIC)
    # Produce fully static executables (no dynamic linker dependency)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
else()
    set(OVMX_LIB_TYPE SHARED)
endif()

# Mode 2 self-hosting (OVMX_LINK_NATIVE, src/vmslink/CMakeLists.txt) is a
# host-arch-only (aarch64/x86_64 musl) build of the VMS-native LINK.EXE
# shareable-image graph, orthogonal to the Mode-1 cross bootstrap build this
# file configures (docs/design-unified-cross-build.md §4 -- the two modes
# must never collapse). It already self-selects off for any non-musl compiler
# target via the -dumpmachine probe below, but force it off explicitly here,
# before src/vmslink is added, so a cross configure can never attempt the
# native LINK.EXE graph even if -DOVMX_LINK_NATIVE=ON is passed by mistake.
if(CMAKE_CROSSCOMPILING)
    set(OVMX_LINK_NATIVE OFF CACHE BOOL
        "Forced OFF under cross-compiling -- Mode 2 self-hosting is host-arch only (vms-509 Mode 1/Mode 2 split)"
        FORCE)
endif()

# --- IMGACT.EXE image-activation build mode --------------------------------
# When ON, all shareable images and executables are laid out so IMGACT.EXE (a
# clean-room musl ELF interpreter, bead vms-913.x) can activate them:
#   * executables carry PT_INTERP = IMGACT.EXE (the --dynamic-linker path);
#   * every ELF object uses DT_HASH (--hash-style=sysv) — IMGACT implements
#     SysV hash lookup only, not GNU_HASH.
# IMGACT also handles only classic RELATIVE/GLOB_DAT/JUMP_SLOT/TLSDESC relocs,
# so images MUST NOT be linked with -z pack-relative-relocs (DT_RELR). The musl
# toolchain does not emit DT_RELR by default; we simply never opt in, and the
# IMGACT activation proof (CI job imgact-proof) asserts RELATIVE-only relocs.
# STARTUP.EXE (PID 1) and IMGACT.EXE itself stay static — see their targets.
if(OVMX_IMGACT)
    # SysV hash on both executables and shareable images.
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -Wl,--hash-style=sysv")
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--hash-style=sysv")

    # PT_INTERP = IMGACT.EXE for every executable.
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -Wl,--dynamic-linker=${OVMX_IMGACT_PATH}")

    # TLS dialect: IMGACT resolves ONLY the TLSDESC relocation family (see
    # docs/design-image-activation.md section 10 + each arch's imgact_arch.h);
    # it does NOT implement the general-dynamic __tls_get_addr / DTPMOD / DTPOFF
    # path. The compiler must therefore emit TLSDESC, not the GD model.
    #   * aarch64: GCC already defaults to the TLSDESC (`desc`) dialect, so no
    #     flag is needed — the aarch64 IMGACT proof (CI) exercises TLSDESC.
    #   * x86_64:  GCC defaults to the GD model (__tls_get_addr); TLSDESC must be
    #     requested explicitly with -mtls-dialect=gnu2 (bead vms-be5). Without
    #     this, x86_64 shareables emit R_X86_64_DTPMOD64/DTPOFF64 + calls to
    #     __tls_get_addr, which the x86_64 IMGACT backend cannot activate.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mtls-dialect=gnu2")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mtls-dialect=gnu2")
    endif()
endif()

# Find required packages
find_package(Threads REQUIRED)

if(BUILD_TESTS)
    enable_testing()
endif()

# Known Image Database lookup module (bead vms-913.5) — standalone,
# mmap(MAP_SHARED)-based O(1) SONAME lookup for IMGACT.EXE's known-image
# search path (docs/design-image-activation.md section 6). Plain POSIX C,
# no freestanding/musl constraint, so it builds unconditionally (independent
# of OVMX_IMGACT) and is exercised by both its own unit test and the
# INSTALL utility's integration test (src/install/). Wiring
# known_images_lookup() into src/imgact/imgact.c's activation search path
# is a deliberate follow-up tracked separately — imgact.c is owned by a
# concurrent bead (vms-913.11).
add_library(known_images STATIC src/imgact/known_images.c)
target_include_directories(known_images PUBLIC src/imgact)

# Shared OVMX kit-container reader (bead vms-df9, docs/design-ovmx-kit-
# format.md). Plain POSIX C, no freestanding constraint, builds
# unconditionally like known_images above -- both tools/ovmx_kit_pack
# (factory build tooling, BUILD_TOOLS-gated) and PRODUCT.EXE (hosted
# SYS$SYSTEM: utility, also BUILD_TOOLS-gated) link it, so it must exist
# before either add_subdirectory below.
add_library(ovmx_kit_reader STATIC src/product/ovmx_kit_reader.c)
target_include_directories(ovmx_kit_reader PUBLIC
    src/product
    src/libvms/include
)

if(BUILD_TESTS)
    add_executable(test_known_images src/imgact/test/test_known_images.c)
    target_link_libraries(test_known_images PRIVATE known_images)
    add_test(NAME known_images_unit COMMAND test_known_images)
    set_tests_properties(known_images_unit PROPERTIES LABELS "unit;imgact" TIMEOUT 30)

    # Proves /SHARED images share physical pages across processes via
    # /proc/PID/smaps (INSTALL done condition #4). See the file header in
    # test_shared_pages.c for the full verification method.
    add_executable(test_known_images_shared_pages src/imgact/test/test_shared_pages.c)
    target_link_libraries(test_known_images_shared_pages PRIVATE known_images)
    add_test(NAME known_images_shared_pages COMMAND test_known_images_shared_pages)
    set_tests_properties(known_images_shared_pages PROPERTIES
        LABELS "unit;imgact"
        TIMEOUT 30
    )
endif()

# Subdirectories (order matters for dependencies)
add_subdirectory(src/libvmssys)
add_subdirectory(src/vmsprocess)
add_subdirectory(src/vmslnm)
add_subdirectory(src/vmsfs)
add_subdirectory(src/libvms)
add_subdirectory(src/vmsrms)
add_subdirectory(src/vmsqueue)
add_subdirectory(src/vmsdcl)

# vmsscs - SCS Datalink daemon (SCSD, docs/design-cluster-node.md sec 3.1).
# No dependency on the OVMX library chain above (plain POSIX AF_PACKET
# sockets), so it builds unconditionally like known_images.
add_subdirectory(src/vmsscs)

# NetBSD/vax facility guest test tools (vmseflag/vmsaccess/vmsproctab/vmsmbx
# + their width-safety negctl teeth) -- vms-74b, epic vms-509 "unified
# cross-platform build" Rung D, docs/design-unified-cross-build.md §5/§8.
# tests/netbsd/guest/CMakeLists.txt no-ops itself (return()) unless
# CMAKE_SYSTEM_NAME is NetBSD, so this is independent of BUILD_TESTS/BUILD_TOOLS
# -- it replaces the four hand-maintained
# tools/cross-vax/build-{eflag,access,proctab,mbx}-tool-vax.sh scripts, which
# always built regardless of any flag either.
#
# EXISTS-guarded (not unconditional): some builds configure the top-level
# project against a partial checkout that copies only src/ + CMakeLists.txt and
# NOT tests/ -- notably the behavioural mutation-sandbox negctls
# (tests/integration/test_show_*_negctl.sh: `cp -a src`, `cp CMakeLists.txt`,
# then `cmake -S <tree>`). An unconditional add_subdirectory() of an absent dir
# is a hard configure error there. The guard keeps the guest tools building on
# any full checkout (native no-ops via the subdir's own NetBSD guard; vax builds
# them) while staying invisible to a src-only sandbox build.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/netbsd/guest/CMakeLists.txt")
    add_subdirectory(tests/netbsd/guest)
endif()

if(BUILD_TESTS)
    add_subdirectory(tests/libvms)
    add_subdirectory(tests/vmsprocess)
    add_subdirectory(tests/vmslnm)
    add_subdirectory(tests/vmsfs)
    add_subdirectory(tests/ods2)
    add_subdirectory(tests/vmsrms)
    add_subdirectory(tests/vmsqueue)
    add_subdirectory(tests/dcl)
    add_subdirectory(tests/vmsscs)
    add_subdirectory(tests/ovmx_init)
    add_subdirectory(tests/tools)

    # Toolchain round-trip: LIBRARIAN.EXE + .OLB + LINK.EXE selective member
    # extraction (vms-ca9). Needs the host-built toolchain binaries, so it is
    # only registered when BUILD_TOOLS is on (src/vmslink is BUILD_TOOLS-gated).
    if(BUILD_TOOLS)
        add_subdirectory(tests/toolchain)
    endif()

    # tests/vmsssh: term_mapping unit test only (vms-97d). No libssh
    # dependency — unlike src/vmsssh (the daemon), this must build and run
    # unconditionally so ctest never reports a skip here.
    add_subdirectory(tests/vmsssh)

    # Public sys$ API tests for the QEMU kernel-executive harness (vms-1d9,
    # epic vms-6b8 Phase 0.5). Links real src/libvms against a real
    # /dev/vms when present; SKIP (ctest exit 77), never a fake PASS,
    # when it is not (see tests/qemu/CMakeLists.txt).
    add_subdirectory(tests/qemu)

    # A full local `ctest` run used to be dominated by three negctl gates
    # (`slow`-labelled, vms-819d): kif_caller_census_negctl,
    # userspace_service_register_negctl, terminal_identity_negctl. All three
    # are TORN OUT / de-gated (vms-b44, same ruling as vms-49f) — each
    # reran a SOURCE-SCAN gate against a mutated sandbox copy of the tree,
    # proving only that the gate can be tripped by known evasions of itself
    # (a string relation over the tree's own self-declaration), never
    # touching vms.ko, QEMU, or /dev/vms. Their scripts remain in
    # tests/integration/ as manual, non-gating lint; they are no longer
    # registered with add_test() and no longer carry the `slow` label. The
    # `slow` label still exists for tests/qemu's real QEMU-boot tests (e.g.
    # parts_demo_e2e) — `ctest -LE slow` remains meaningful for those.
    #
    # `ctest --timeout N` on the command line CANNOT be used to bound a slow
    # test that still carries its own TIMEOUT property:
    # it is documented as a DEFAULT, used only for tests that carry no
    # explicit CMake TIMEOUT property. Every gate below sets its own TIMEOUT,
    # so it always wins over the CLI flag regardless of N — this was
    # MEASURED directly (a two-test throwaway CMake project, one test with a
    # TIMEOUT property and one without, both running `sleep 30`: `ctest
    # --timeout 3` killed the one without the property at 3.01s and let the
    # one with TIMEOUT 1800 run to completion at 30.00s) and independently
    # corroborated in docs/HANDOFF-vms-14f.md's vms-008 note ("`ctest
    # --timeout 120` does **not** override a per-test `TIMEOUT` property").
    # This is CMake/CTest's documented precedence, not a signal that fails to
    # reach a process group, and not a defect in any script here — do not
    # go looking for one. A genuine external ceiling on a full run needs
    # `ctest -LE slow` (scope out what's slow) or wrapping the whole `ctest`
    # invocation in `timeout(1)`/`--stop-time`, not `--timeout`.

    # INV-1 standing gate (vms-e652): system identity has ONE owner, and the
    # login banner stays driven by SYS$WELCOME rather than a compiled-in
    # printf. Source scan — no build artifacts needed.
    add_test(NAME identity_ssot_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_identity_ssot.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(identity_ssot_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # Dual-identity rebrand tripwire (rd vms-700/vms-296/vms-3de): the
    # OpenVMX product rebrand must NOT touch the "OVMX" tokens that are
    # load-bearing as non-brand machine identifiers -- the OVMX$_ facility
    # name, the IMGACT ELF-note owner, the SCS wire OS-name field, nodename
    # fallbacks, and the kit/product-db vendor token. Source scan.
    add_test(NAME frozen_identity_tokens_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_frozen_identity_tokens.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(frozen_identity_tokens_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # Rule 9 standing gate: ONE runtime target (kernel/QEMU). Docker is not a
    # runtime, and no executive facility may silently fall back to a
    # per-process userspace fake when /dev/vms is absent. Source scan.
    add_test(NAME runtime_target_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_runtime_target.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(runtime_target_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # vms-8d2 standing gate: PID 1 does not run the console login loop --
    # JOB_CONTROL, a DETACHED process, does. Source scan — no build
    # artifacts needed. The end-to-end proof (real boot, real detached
    # process, SHOW SYSTEM lists it, SYSTEM logs in through it) is
    # tests/qemu/test_job_control_console.sh, opt-in like the other real-
    # boot gates.
    add_test(NAME job_control_ownership_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_job_control_ownership.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(job_control_ownership_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # vms-2a9 standing gate: JOB_CONTROL is created by a registered STDRV
    # component (SYS$STARTUP:VMS$VMS.DAT, CONFIG phase), not a hardcoded
    # call from a site file. Source scan — no build artifacts needed. The
    # end-to-end proofs are tests/qemu/test_job_control_console.sh (vms-8d2,
    # positive) and tests/qemu/test_job_control_negctl.sh (vms-2a9,
    # negative), both opt-in like the other real-boot gates.
    add_test(NAME job_control_component_registration_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_job_control_component_registration.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(job_control_component_registration_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # The Rule 9 gate is a lint, and twice it certified the very regression it
    # exists to catch (see the file header). This runs the recorded evasions
    # against it on a sandbox copy of the tree and requires each one to drive
    # it RED for its own specific reason. A gate nobody tries to evade is an
    # assertion about nothing.
    add_test(NAME runtime_target_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_runtime_target_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(runtime_target_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 120)

    # vms-bae standing gate: drivers/ovmx/ is a REUSABLE kernel-module home, not
    # a hard-coded {vms,vmsfs} pair. Runs the real overlay against a fabricated
    # kernel tree with a hypothetical third module dropped in as {subdir +
    # Kconfig stanza + config flag} only, and proves it is discovered and fully
    # wired (menu sourced, Makefile obj- line, sources flattened) with the
    # overlay script byte-for-byte unchanged. Hermetic source/wiring proof -- no
    # kernel build; the real intree=Y + signed proof on the shipped modules is
    # the boot-time gate tests/qemu/test_kernel_taint.sh.
    add_test(NAME ovmx_module_home_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_ovmx_module_home.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(ovmx_module_home_gate PROPERTIES
        LABELS "integration;kernel" TIMEOUT 120)

    # vms-bd1 standing gate: the b65 shareable symbol vectors (LIBVMSSYS$SHR
    # vms_kif_* + LIBVMS$SHR universals) are FROZEN and APPEND-ONLY, so GSMATCH
    # LEQUAL stays sound once vmsrms/DCL bind universal indices. Asserts the
    # committed golden (src/vmslink/*.vec.frozen) is still an exact ordered
    # prefix of the live manifest (no reorder/drop), no duplicate names, and no
    # harness re-grew an inline copy of the vector. Pure source scan -- no build.
    add_test(NAME symvec_freeze_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_symvec_freeze.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(symvec_freeze_gate PROPERTIES
        LABELS "integration;authenticity;toolchain" TIMEOUT 60)

    # Negative control for the above -- proves the freeze gate reddens on reorder,
    # drop, duplicate, and a re-grown inline vector copy, and stays green on a
    # legal append, in a disposable synthetic source tree.
    add_test(NAME symvec_freeze_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_symvec_freeze_negctl.sh)
    set_tests_properties(symvec_freeze_negctl PROPERTIES
        LABELS "integration;authenticity;toolchain" TIMEOUT 60)

    # vms-4847 standing gate: every *_no_python3.cmake path a CMakeLists.txt
    # else() branch names must resolve to a file `git ls-files` actually
    # tracks. The blanket `*.cmake` .gitignore rule swallowed three (later
    # five) of these shims for months -- most silently, one loudly. The
    # .gitignore carve-out (`!*_no_python3.cmake`) fixes the re-ignored case;
    # this gate re-derives, on every run, that the fix held and that a shim
    # was not simply never `git add`-ed in the first place.
    add_test(NAME cmake_shims_tracked_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_cmake_shims_tracked.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(cmake_shims_tracked_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # Negative controls for the above -- proves it reds for both ways a shim
    # can go missing (untracked-on-disk, absent-on-disk) and stays green for
    # the innocent case, in a disposable sandbox git repo.
    add_test(NAME cmake_shims_tracked_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_cmake_shims_tracked_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(cmake_shims_tracked_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # vms-55a (epic vms-a84 RELEASE ENGINEERING): docs/install-guide.md and
    # docs/upgrade-guide.md must not silently drift from the e2e gates that
    # actually prove them (tests/qemu/test_product_install_e2e.sh,
    # tests/qemu/test_upgrade_e2e.sh). Pure source/text scan via
    # tools/check_guide_drift.py -- no docker, no QEMU.
    add_test(NAME guide_drift_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_guide_drift.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(guide_drift_gate PROPERTIES
        LABELS "integration;release" TIMEOUT 60)

    # Negative controls for the above -- proves the comparator actually goes
    # red on real drift (wrong command text, a dropped step, a reordering,
    # and a reverted kit name), and distinguishes that from the "no
    # GUIDE-STEPS block at all" fixture-error path. See the file header for
    # why an always-OK checker would make the gate above vacuous.
    add_test(NAME guide_drift_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_guide_drift_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(guide_drift_negctl PROPERTIES
        LABELS "integration;release" TIMEOUT 60)

    # vms-644 (epic vms-a84 RELEASE ENGINEERING): tools/publish-release.sh takes
    # a cut bundle and publishes its artifacts + generated notes to a GitHub
    # Release. This gate runs the publisher inside a throwaway git repo against
    # a stub `gh` -- no GitHub, no network -- and asserts BOTH the happy path
    # (all assets attached, generated notes used as the body, notes recorded
    # in-tree) AND the negative controls that make it non-vacuous: a corrupt
    # bundle or a tag that disagrees with the bundle is refused before any gh
    # call. Pure shell; no docker, no QEMU.
    add_test(NAME publish_release_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_publish_release.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(publish_release_gate PROPERTIES
        LABELS "integration;release" TIMEOUT 60)

    # vms-644b: gate for tools/gen_release_notes.py's previous-release-tag
    # detection. Builds a throwaway git repo with two release tags and proves
    # that cutting the NEWER tag by name (--ref <tag>, the release.yml tag-push
    # publish path) spans <prev-tag>..<tag> and lists the commits between them
    # -- NOT the empty <tag>..<tag> range that shipped zero-commit notes. Its
    # negative control asserts the empty-range regression cannot reappear. Pure
    # shell + git; no docker, no QEMU, no network.
    add_test(NAME gen_release_notes_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_gen_release_notes.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(gen_release_notes_gate PROPERTIES
        LABELS "integration;release" TIMEOUT 60)

    # vms-791 (0.4 installer Tier-0): the VMSFS media-mastering tool must
    # produce an image that reads back byte-exact. Masters a synthetic tree
    # (subdirs, a >5-entry directory, a multi-block file, a binary file, an
    # empty file) and the real distro/rootfs/vms tree, then extracts each and
    # diffs. Guarded on BUILD_TOOLS (the vmsfs_master target is defined later,
    # when add_subdirectory(tools) runs; the generator expression below is
    # resolved at generate time, after that target exists).
    if(BUILD_TOOLS)
        add_test(NAME vmsfs_master_roundtrip
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_vmsfs_master_roundtrip.sh
                    $<TARGET_FILE:vmsfs_master>
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(vmsfs_master_roundtrip PROPERTIES
            LABELS "integration;tools" TIMEOUT 120)

        # vms-d9c: the OVMX/NetBSD-vax SYSTEM volume must master to a rooted,
        # bootable [SYS0.SYSCOMMON.SYSEXE] layout (the shape ovmx_init's
        # require_installed_system() / run_startup() resolve), and the staged
        # SYS$MANAGER:SYSTARTUP_VMS.COM must be the Decision-A vax variant with
        # NO INSTALL ADD SYS$SHARE block. Host, per-PR half of vms-d9c (the SIMH
        # boot is the nightly netbsd-vax-sysboot job); shares the same host
        # vmsfs_master the roundtrip test uses.
        add_test(NAME sysvol_master_vax
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_sysvol_master_vax.sh
                    $<TARGET_FILE:vmsfs_master>
                    ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(sysvol_master_vax PROPERTIES
            LABELS "integration;tools;netbsd-vax" TIMEOUT 120)
    endif()

    # vms-0b6: the OVMX kit packer (tools/ovmx_kit_pack.c) must produce a kit
    # file that reads back byte-exact. Packs a synthetic SYSEXE/SYSLIB/SYSMGR/
    # SYSHLP tree (nested subdir, empty dir, empty file, binary file, a
    # no-type file), checks the `list` manifest names every staged file, then
    # extracts and byte-compares every payload file. Guarded on BUILD_TOOLS
    # for the same generator-expression-timing reason as vmsfs_master above.
    if(BUILD_TOOLS)
        add_test(NAME ovmx_kit_pack_roundtrip
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_ovmx_kit_pack_roundtrip.sh
                    $<TARGET_FILE:ovmx_kit_pack>)
        set_tests_properties(ovmx_kit_pack_roundtrip PROPERTIES
            LABELS "integration;tools" TIMEOUT 60)

        # vms-c2c: the same packer, pointed at a synthetic VAX kit payload
        # (docs/design-vax-installer.md §4 -- five boot images + four utility
        # images + SYSUAF/RIGHTSLIST/OVMXVMSSYS.PAR + STARTUP.COM + the
        # Decision-A SYSTARTUP_VMS.COM), must round-trip byte-exact and carry
        # the "OVMX VAXVMS VMS" product identity. No vax cross-build toolchain
        # needed -- ovmx_kit_pack is a host tool and this proves the
        # packaging PATH, not the real vax images (tools/cross-vax/
        # build-os-kit-vax.sh drives the real cross-built payload).
        add_test(NAME ovmx_kit_pack_vax_roundtrip
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_ovmx_kit_pack_vax_roundtrip.sh
                    $<TARGET_FILE:ovmx_kit_pack>)
        set_tests_properties(ovmx_kit_pack_vax_roundtrip PROPERTIES
            LABELS "integration;tools;netbsd-vax" TIMEOUT 60)
    endif()

    # vms_kif caller census (vms-7fb): every kernel-interface entry point is
    # either reached from the product or declared unwired against an item.
    # The Rule 9 gate is shaped to catch a FALLBACK; a facility implemented
    # wholly in userspace never mentions /dev/vms at all, so nothing caught the
    # ABSENCE — four interfaces shipped with zero callers and were therefore
    # never once exercised. Source scan.
    add_test(NAME kif_caller_census
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_kif_caller_census.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    # TIMEOUT raised from 120 (vms-e2b): the census now configures cmake and
    # preprocesses every product translation unit instead of globbing source
    # text. ~9s on workshop; the headroom is for slower CI runners.
    set_tests_properties(kif_caller_census PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 600)

    # ...and the evasions, run against a sandbox copy of the tree — TORN OUT
    # / de-gated (vms-b44, same ruling as vms-49f / CLAUDE.md Rule 9). This
    # mutated the SOURCE tree and reran test_kif_caller_census.sh (a source
    # scan) against the mutant, asserting the scan went RED for the expected
    # reason. That proves the census can be tripped by known evasions of
    # itself — a string relation over the tree's own self-declaration — and
    # never booted vms.ko, ran QEMU, or touched /dev/vms. No longer
    # registered with add_test(); the script stays in tree at
    # tests/integration/test_kif_caller_census_negctl.sh and can still be run
    # by hand (`sh tests/integration/test_kif_caller_census_negctl.sh`) as
    # informational, non-gating lint.

    # The citation-ledger apparatus that used to sit here (rd_citations_fresh,
    # rd_citations_partial_list, tracking/rd-citations.tsv, tools/gen_rd_citations.py)
    # is torn down (rd vms-dc7, operator ruling 2026-08-06): it verified that a
    # source comment cited an rd item id that resolved open in a committed
    # snapshot, never anything about whether the labeled claim was true, and it
    # could not see the ledger go stale between regenerations -- exactly the
    # failure mode that let sys_lock.c cite a closed item and still print PASS
    # (vms-344). The real proof for wired facilities remains the QEMU
    # negative-control harness below; nothing here replaces or weakens it.

    # The QEMU harness reaches the right verdict (vms-b1f). run_tests.sh used
    # to decide "did this run report zero failed suites?" with a pipeline under
    # `set -o pipefail`: grep -q exits on match without draining, echo takes
    # SIGPIPE, the pipeline reports 141, and the harness printed KERNEL MODULE
    # TESTS FAILED on a run with ZERO failures. It needed more than one pipe
    # buffer of output AFTER the matching line, which is why a small transcript
    # never showed it. These controls execute the verdict against synthetic
    # transcripts — no QEMU, no container — so the decision is testable on any
    # host, including the ones where run_facility_negctl.sh cannot run at all.
    add_test(NAME qemu_harness_verdict
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_qemu_harness_verdict.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(qemu_harness_verdict PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 120)

    # Userspace service register (vms-5b4): every sys$ service either reaches
    # the executive or is declared, against an item, as answering from
    # userspace. The census asks "is this vms_kif wrapper called?" and cannot
    # ask "is there another implementation of the same public service the
    # product reaches INSTEAD" — a facade is invisible to a gate that only
    # counts calls to the real thing. Source scan.
    #
    # NOT a pure source scan any more (vms-f26): it COMPILES every product .c
    # file it scans and reads the exported sys$ symbols out of the objects with
    # nm, because the source-text readings of the universe were all escaped
    # together by an asm-label rename that left the object symbol-identical.
    # One compile per product .c file per run, hence the raised timeout.
    #
    # AND IT RUNS CMAKE ITSELF (vms-c19). Which files it reads is no longer a
    # directory glob: it configures this project into a throwaway directory and
    # unions the glob with the translation units compile_commands.json says the
    # product compiles, because a definition RELOCATED out of src/ and tools/
    # left every reading at once while CMake went on shipping it. Running this
    # gate therefore needs cmake on PATH; ctest already implies that. Measured
    # cost of the extra configure: 0.6s per run.
    add_test(NAME userspace_service_register
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_userspace_service_register.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(userspace_service_register PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 300)

    # ...and the evasions, on a sandbox copy of the tree — TORN OUT / de-gated
    # (vms-b44, same ruling as vms-49f / CLAUDE.md Rule 9). The script's own
    # header already said what this is: each control mutates the SOURCE and
    # reruns test_userspace_service_register.sh's symbol scan against the
    # mutant, including edits to OVMX-EXECUTIVE citation comments that name a
    # proof file under tests/qemu/. Quoting the script's own header verbatim:
    # "What none of this executes is the OVMX PRODUCT: no vms.ko, no QEMU, no
    # /dev/vms, nothing that could tell an executive-resident answer from a
    # per-process one." A citation comment matching a filename is a string
    # relation, not proof the cited file was ever run against a real
    # executive. The proof that DOES touch the product is
    # tests/qemu/run_facility_negctl.sh, unaffected by this change. No longer
    # registered with add_test(); the script stays in tree at
    # tests/integration/test_userspace_service_register_negctl.sh and can
    # still be run by hand as informational, non-gating lint.

    # Rule 10/11 standing gate (vms-fb9): a process does not name its own
    # terminal, and SHOW DEVICE does not invent device rows. Keeps the
    # VMS_TERMINAL / VMS_DEVICE_TYPE environment handoff, the private _FTA
    # name pool and SHOW DEVICE's /proc/mounts + process-local row sources
    # deleted. Source scan, C comments stripped.
    add_test(NAME terminal_identity_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_terminal_identity.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(terminal_identity_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # ...and the evasions, one minimal mutation per property, on a sandbox
    # copy of the tree — TORN OUT / de-gated (vms-b44, same ruling as vms-49f
    # / CLAUDE.md Rule 9). This reran test_terminal_identity.sh (a source
    # scan, C comments stripped) against a mutated sandbox copy of src/ and
    # asserted the scan went RED for the expected reason -- a string relation
    # over the tree's own self-declaration, no vms.ko, no QEMU, no /dev/vms.
    # No longer registered with add_test(); the script stays in tree at
    # tests/integration/test_terminal_identity_negctl.sh and can still be run
    # by hand as informational, non-gating lint.

    # Rule 11 standing gate (vms-a17e): PID 1 / PROVISION.EXE constructs no
    # identity from SYSUAF. vms.ko owns SYSTEM's identity as its own
    # constants (VMS_SYSTEM_UIC / VMS_PRV_M_SYSTEM_ALL), the OPA0: device-
    # table precedent applied to identity -- see vms_ioctl_establish_system()
    # in src/kernel/vms_proctab.c. Source scan, C comments stripped.
    add_test(NAME system_identity_no_sysuaf_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_system_identity_no_sysuaf.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(system_identity_no_sysuaf_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # ...and the evasions, one minimal mutation per property, on a sandbox
    # copy of the tree -- no vms.ko, no QEMU, no /dev/vms. The BEHAVIOURAL
    # proof against a real /dev/vms is tests/qemu/test_release_e2e.sh's
    # "poisoned_uic" case (a real boot with a SYSUAF SYSTEM row carrying
    # UIC [50,50]/PRIVILEGES=NONE still reports "SYSTEM [1,4] established").
    add_test(NAME system_identity_no_sysuaf_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_system_identity_no_sysuaf_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(system_identity_no_sysuaf_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # Rule 10/11 standing gate (vms-cb5): the DERIVED census of every writer
    # and reader of the identity environment variables, across src/ AND
    # tools/. The gate above scans src/ only, and both the reachable writer
    # (tools/vms_login.c) and the single surviving reader (tools/vms_mail.c)
    # live in tools/ -- which is how a security review came to state, and
    # nearly ship, "written ONLY by vmssshd". A prose census drifts silently;
    # this one fails loudly, and prints itself either way.
    add_test(NAME env_identity_census
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_env_identity_census.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(env_identity_census PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # ...and the evasions. Includes the two controls that matter most for a
    # gate whose passing output is a LIST: that tools/ is genuinely scanned,
    # and that a variable named only in a comment cannot enter the census.
    add_test(NAME env_identity_census_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_env_identity_census_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(env_identity_census_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 180)

    # The BEHAVIOURAL half of the same rule (vms-fb9 round 2), and the one
    # that actually carries the invariant. The two source scans above check
    # that named tokens are absent; an adversary reintroduced SHOW DEVICE's
    # hardcoded stub row written in the CURRENT oracle format and every one
    # of those checks passed. This RUNS DCL and requires stdout to contain no
    # device row at all when there is no executive to have read one from --
    # which no re-spelling of the fabrication can satisfy.
    add_test(NAME show_device_rows_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_show_device_rows.sh
                $<TARGET_FILE:vmsdcl>)
    set_tests_properties(show_device_rows_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # ...and its evasions, which BUILD mutant DCLs (including the exact
    # adversary mutant) in a sandbox and require each to drive the gate red.
    # A behavioural gate needs a behavioural proof that it can go red.
    add_test(NAME show_device_rows_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_show_device_rows_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(show_device_rows_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 300)

    # SHOW CLUSTER reflects the SCS daemon's LIVE membership (rd vms-8d4,
    # INV-DCL). cmd_show_cluster() used to hardcode %SYSTEM-I-NOTMEMBER, so a
    # genuinely-clustered node lied. This RUNS DCL against a membership file
    # written in SCSD's own publication format (scs_membership.h -- the real
    # boundary between the daemon and the DCL process) and requires the member
    # set to be shown when present and NOTMEMBER only when genuinely absent.
    add_test(NAME show_cluster_membership_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_show_cluster_membership.sh
                $<TARGET_FILE:vmsdcl>)
    set_tests_properties(show_cluster_membership_gate PROPERTIES
        LABELS "integration;authenticity;cluster" TIMEOUT 60)

    # SHOW SYMBOL renders an integer's Hex and Octal columns as a LONGWORD
    # (rd vms-c71), pinned to a side-by-side measurement against OpenVMS VAX
    # V7.3. Two defects lived on one line: a negative sign-extended to 16 hex
    # digits where VMS prints 8, and the octal field was padded to 12 digits
    # where a longword is 11 -- the latter wrong for EVERY integer symbol,
    # including positive ones, which is why it survived (the hex column looks
    # correct for positives).
    add_test(NAME show_symbol_longword_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_show_symbol_longword.sh
                $<TARGET_FILE:vmsdcl>)
    set_tests_properties(show_symbol_longword_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # ...and the proof that each of its four assertions carries a defect. Three
    # mutants are built and RUN, and each declares the COMPLETE set of gate
    # checks it expects to redden, compared for EQUALITY -- an unlisted red
    # fails the control just as a listed green does. Case C (octal fixed, sign
    # extension left) is the one that keeps this gate from silently becoming
    # positive-only, which is the state the tree was in before a negative value
    # was reachable through F$IDENTIFIER at all.
    #
    # TIMEOUT derived, not guessed, and the measurement says under WHAT LOAD --
    # which is the half that is usually missing. MEASURED on workshop
    # 2026-08-05: 7-8s wall-clock serialized (invoked directly, and under ctest
    # with nothing else running), 14s with a second full ctest running
    # concurrently. One sandbox configure plus four builds of the vmsdcl target.
    # 120 is ~8.5x the loaded figure. A bare "8s" would have been true and
    # would have overstated the margin by 2x, because this suite's cost is
    # dominated by compiler contention rather than by its own work -- the same
    # effect that took the per-facility negctl job from 27m to >60m and killed
    # it (rd vms-86a). If this is ever approached, measure and record it here
    # rather than doubling it blind (the lesson rd vms-008 charged to
    # terminal_identity_negctl, where a never-derived 120 was killing a
    # PASSING test).
    add_test(NAME show_symbol_longword_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_show_symbol_longword_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(show_symbol_longword_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 120)

    # The OPCOM record OVMX writes carries the MESSAGE (rd vms-2d37). $SNDOPR
    # takes an OPC message BLOCK and the service used to read it as a C string,
    # so it stopped at the first NUL inside the header and every record's body
    # was two control bytes -- an audit trail that recorded that a request
    # happened, by whom and when, and never what it was.
    #
    # BEHAVIOURAL, not a source scan: it runs DCL, makes it emit a record, and
    # reads what landed in OPERATOR.LOG. The body checks are presence-based, so
    # a service that writes no body at all fails rather than passing by the
    # absence of the bad bytes.
    add_test(NAME opcom_record_body_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_opcom_record_body.sh
                $<TARGET_FILE:vmsdcl>)
    set_tests_properties(opcom_record_body_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60
        # vms-ed4: reads a GLOBAL recency-ordered OPERATOR.LOG view across all of
        # /vms and /tmp (find /vms /tmp -iname OPERATOR.LOG -newer $STAMP). Any
        # other test emitting an OPERATOR.LOG concurrently could poison that
        # scan, so under ctest --parallel it must run alone. RUN_SERIAL, not a
        # sysdisk lock, because the shared surface includes /tmp too (Rule 8).
        RUN_SERIAL TRUE)

    # MONITOR PROCESS and SHOW SYSTEM list the SAME process set because they
    # read the SAME single source -- the executive process table via
    # vms_kif_procscan (rd vms-c840). MONITOR used to build its list from its
    # own /proc walk (every Linux task), so the two commands diverged
    # (operator-observed 2026-08-14). This RUNS both binaries and requires
    # their PID sets to be EQUAL, plus proves MONITOR actually read the
    # executive (strace openat("/dev/vms")) so an empty host set is not a
    # vacuous pass. BUILD_TOOLS-gated: MONITOR.EXE is a tools/ target.
    if(BUILD_TOOLS)
        add_test(NAME monitor_process_source_gate
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_monitor_process_source.sh
                    $<TARGET_FILE:vms_monitor> $<TARGET_FILE:vmsdcl>)
        set_tests_properties(monitor_process_source_gate PROPERTIES
            LABELS "integration;authenticity" TIMEOUT 120)

        # ...and the proof it can go red: a sandbox MONITOR with the /proc scan
        # reintroduced must drive the gate RED (positive control passes first).
        # Builds one tool target in a sandbox -- cmake required.
        add_test(NAME monitor_process_source_negctl
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_monitor_process_source_negctl.sh
                    ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE:vmsdcl>)
        set_tests_properties(monitor_process_source_negctl PROPERTIES
            LABELS "integration;authenticity" TIMEOUT 300)
    endif()

    # Divider integrity (vms-6d7): catch git's 3-way auto-merge silently
    # truncating/fusing comment-divider blocks -- the vms-371 rebase
    # incident (2e30da9 / 2dcd237), corrupted OUTSIDE the region git
    # reported as needing manual resolution, invisible to py_compile
    # (comments) and to the wire/data tests (they string-search a markdown
    # spec, not comment formatting). Source scan over tracked .py/.c/.h;
    # runs its own proof suite first (synthetic corrupted + clean fixtures)
    # so the sweep is only trusted once the detector proves itself.
    add_test(NAME divider_integrity_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_divider_integrity.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(divider_integrity_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 120)

    # Cluster-identity uniqueness (vms-1ae): SCSNODE/SCSSYSTEMID are
    # cluster-wide unique keys, and a joiner reusing either one that the peer
    # already knows is refused by the configuration poller ("%PEA0, Remote
    # System Conflicts with Known System") in a way that looks exactly like the
    # vms-2f3 stall. Two agents had already collided on SCSSYSTEMID 1601 and
    # 1602. Hermetic -- builds its own SYSG v2 stores in a temp dir, needs no
    # lab, no k3s and no VAX -- and carries both directions plus a control that
    # the guard is reading the store registry rather than rejecting on sight.
    add_test(NAME lab_identity_unique_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_lab_identity_unique.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(lab_identity_unique_gate PROPERTIES
        LABELS "integration;cluster" TIMEOUT 60)

    # x86_64 relocation survey freshness (vms-680, under vms-bdf): the
    # committed docs/design-link-x86_64-relocs.md is GENERATED, not hand-typed
    # -- tools/survey_x86_64_relocs.sh cross-compiles the vms-b65.1-.6 object
    # set to x86_64 .o files with musl-gcc -fPIC -mtls-dialect=gnu2 and tallies
    # every observed ELF r_type by name and count. `--check` regenerates into a
    # temp file and diffs against the committed doc; this is that check wired
    # into ctest so the downstream x86_64 reloc-apply items (vms-8f5, vms-cd1,
    # vms-2e4) can't drift from what the compiler actually emits. Skipped if
    # musl-gcc isn't on the host -- same containerized-build-tooling posture as
    # the rest of the x86_64 musl toolchain (CLAUDE.md Rule 9's "build tooling
    # is fine" carve-out).
    find_program(MUSL_GCC_FOR_RELOC_SURVEY musl-gcc)
    if(MUSL_GCC_FOR_RELOC_SURVEY)
        add_test(NAME x86_64_reloc_survey_fresh
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/survey_x86_64_relocs.sh --check)
        set_tests_properties(x86_64_reloc_survey_fresh PROPERTIES
            LABELS "integration;toolchain" TIMEOUT 300)
    else()
        message(STATUS "musl-gcc not found -- x86_64_reloc_survey_fresh test skipped")
    endif()

    # PARTS_SETUP.COM (vms-977, under the vms-2579 0.2 "killer app" demo
    # epic): the pre-PCSI layered-product install procedure for the PARTS
    # demo (src/apps/parts/, vms-e97/vms-f20, #172/#179) -- COPY PARTS.EXE
    # from the install kit (SYS$UPDATE:) to SYS$SYSTEM: and DEFINE PARTS as
    # a foreign command. BEHAVIOURAL: runs the committed .COM through a real
    # DCL.EXE in an isolated VMS_ROOT (never the shared /vms tree), using the
    # host-built PARTS binary as a stand-in image -- COPY/DEFINE do not care
    # about a file's ELF contents. Mastering the VMS-native PARTS.EXE into
    # the bootable image and the full QEMU boot->install->run chain are
    # vms-cde's e2e gate, not this one.
    # PARTS is a BUILD_TOOLS target (src/apps/parts); these two gates use it as
    # a stand-in image, so they only register when the demo target exists. Under
    # BUILD_TESTS with BUILD_TOOLS=OFF (e.g. the cross-alpha lib-stack gate, rd
    # vms-fed) PARTS is absent -- registering the tests then hard-fails Generate
    # on an unresolved $<TARGET_FILE:PARTS>. The comment below already frames
    # PARTS as "if available".
    if(TARGET PARTS)
    add_test(NAME parts_setup_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_parts_setup.sh
                $<TARGET_FILE:vmsdcl> $<TARGET_FILE:PARTS>
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(parts_setup_gate PROPERTIES
        LABELS "integration;parts" TIMEOUT 60)

    # Foreign-command dispatch (vms-96e): "SYM :== $image" then a BARE "SYM
    # args" must activate the image with args as P1-P8, the same as real VMS
    # (LOGIN.COM's "XXX :== $DEV:[DIR]XXX.EXE" pattern). Before this, only
    # "RUN image" reached image activation -- dcl_find_verb() checked ONLY
    # the static builtin_verbs[] table, so a bare foreign-command verb fell
    # straight through to %DCL-W-IVVERB. BEHAVIOURAL: runs DCL.EXE for real
    # against disposable shell-script images built into a temp dir, and (if
    # the PARTS demo target is available) proves the exact "PARTS :==
    # $SYS$SYSTEM:PARTS.EXE" / "$ PARTS" shape end to end.
    add_test(NAME foreign_command_dispatch_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_foreign_command_dispatch.sh
                $<TARGET_FILE:vmsdcl> $<TARGET_FILE:PARTS>)
    set_tests_properties(foreign_command_dispatch_gate PROPERTIES
        LABELS "integration;dcl" TIMEOUT 60)
    endif()  # TARGET PARTS
endif()

# vmssshd requires libssh — skip if headers not available
find_path(LIBSSH_INCLUDE_DIR NAMES libssh/libssh.h)
find_library(LIBSSH_LIBRARY NAMES ssh)
if(LIBSSH_INCLUDE_DIR AND LIBSSH_LIBRARY)
    add_subdirectory(src/vmsssh)
else()
    message(STATUS "libssh not found — vmssshd will not be built")
endif()

# IMGACT.EXE — the ELF interpreter itself (aarch64-only until bead vms-913.11).
if(OVMX_IMGACT)
    add_subdirectory(src/imgact)
endif()

if(BUILD_TOOLS)
    add_subdirectory(tools)
    add_subdirectory(src/ovmx_init)
    add_subdirectory(src/ovmx_provision)  # PROVISION.EXE (startup process, vms-9b7)
    add_subdirectory(src/ovmx_job_control) # JOB_CONTROL.EXE (console-session process, vms-8d2)
    add_subdirectory(src/vmslink)   # LINK.EXE + OVMXDUMP + LIBRARIAN.EXE (VMS-native linker)
    add_subdirectory(src/install)   # INSTALL.EXE (known image database, vms-913.5)
    add_subdirectory(src/product)   # PRODUCT.EXE (PCSI-equivalent installer, vms-df9)

    # PARTS.EXE (vms-e97/vms-f20 demo, vms-64a unified cross-build Rung B):
    # moved here from the BUILD_TESTS block below. On x86_64/aarch64 the
    # SHIPPED PARTS.EXE is the Mode-2 self-hosted LINK.EXE image
    # (src/apps/parts/mk_parts.sh, docs/design-unified-cross-build.md §4) --
    # this CMake target stays the host/dev demo used by ctest. On vax there is
    # no Mode 2 at all (Mode 2 is host-arch-only), so this Mode-1 CMake target
    # IS vax's PARTS.EXE -- the same pattern DCL.EXE/LOGINOUT.EXE already
    # follow (ordinary CMake targets on vax, Mode-2 link-native on x86_64).
    # BUILD_TOOLS-gated (not BUILD_TESTS-gated) so it is part of the shipped
    # image set / ovmx-images aggregate on every toolchain; the functional
    # ctest gate inside src/apps/parts/CMakeLists.txt stays BUILD_TESTS-gated.
    add_subdirectory(src/apps/parts)
endif()

# ovmx-images (vms-64a, epic vms-509 "unified cross-platform build" Rung B,
# docs/design-unified-cross-build.md §3-B): the SINGLE authoritative "what
# ships" list. One custom aggregate target that depends on exactly the
# shipped userspace images (+ the RTL they pull in transitively via
# target_link_libraries) -- the CMake-native replacement for the hand-
# maintained VAX_ARTIFACT_ORDER array in tools/cut-release.sh and the `cp`
# lines in distro/Dockerfile.bootable (rd vms-e1d's parity gate compares
# those two by text-parsing today; the design's stated end state is pointing
# that gate at this target's outputs instead, once every image builds
# through it for every arch -- tracked as a Rung C/E/F follow-up, not done
# by this rung). `cmake --build <dir> --target ovmx-images` builds the whole
# shipped set for whatever arch/toolchain is configured, and NOTHING else
# (no tests, no host factory tooling like vmsfs_master/ovmx_kit_pack, no
# vms_mount_helper). Porting a new image for a substrate = adding its target
# here, never a new tools/cross-vax/build-*.sh script (operator freeze).
if(BUILD_TOOLS)
    set(_OVMX_IMAGES_DEPS
        ovmx_init            # STARTUP.EXE
        ovmx_provision       # PROVISION.EXE
        ovmx_job_control     # JOB_CONTROL.EXE
        vmsdcl                # DCL.EXE
        vms_login            # LOGINOUT.EXE
        vmsdump              # OVMXDUMP
        vmslibrarian         # LIBRARIAN.EXE
        vms_install          # INSTALL.EXE
        vms_product          # PRODUCT.EXE
        vms_help             # HELP.EXE
        vms_authorize        # AUTHORIZE.EXE
        vms_mail             # MAIL.EXE
        vms_monitor          # MONITOR.EXE
        vms_initialize       # INITIALIZE.EXE
        vms_sysgen           # SYSGEN.EXE
        PARTS                # PARTS.EXE
    )

    # SCSD.EXE (scsd_exe, src/vmsscs/CMakeLists.txt): the SCS datalink daemon.
    # vms-838 gave it a thin raw-L2 datalink abstraction (scs_datalink.h/.c,
    # src/vmsscs/) with a Linux AF_PACKET backend (unchanged behavior) and a
    # NetBSD bpf(4) backend (NetBSD has no AF_PACKET; bpf(4) is its raw-link
    # facility, a materially different API), selected at compile time --
    # scsd.c itself no longer opens a socket directly. That closed the last
    # of rd vms-e1d's ten x86_64<->vax parity-drift images, so SCSD.EXE now
    # ships in the aggregate on every substrate, same as everything else in
    # _OVMX_IMAGES_DEPS above.
    list(APPEND _OVMX_IMAGES_DEPS scsd_exe)  # SCSD.EXE

    if(NOT CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
        # LINK.EXE (vmslink, src/vmslink/link.c): the OVMX-native ELF64
        # LINK.EXE hardcodes AArch64/x86_64 ELF64 relocation handling
        # (R_AARCH64_*, Elf64_Section) -- it is the linker that PRODUCES
        # x86_64/aarch64 Mode-2 shareable images (docs/design-unified-
        # cross-build.md §4), a role that has no meaning on vax: Decision-A
        # vax images are ordinary NetBSD ld.elf_so-activated ELF32
        # executables with no LINK.EXE/IMGACT.EXE involvement at all, the
        # same "no VAX role by design" status IMGACT.EXE already carries in
        # tools/parity/image-parity-allowlist.json. Not one of the vms-e1d
        # drift images (LINK.EXE was never in VAX_ARTIFACT_ORDER); a
        # genuine by-design exclusion, not an unbuilt gap.
        list(APPEND _OVMX_IMAGES_DEPS vmslink)  # LINK.EXE
    endif()

    add_custom_target(ovmx-images)
    add_dependencies(ovmx-images ${_OVMX_IMAGES_DEPS})
endif()

# ovmx-images COMPONENT-scoped install (vms-88c, epic vms-509 "unified
# cross-platform build" Rung F, docs/design-unified-cross-build.md §3-B
# follow-up: "point the vms-e1d parity gate at this target's outputs instead
# [of the hand-maintained VAX_ARTIFACT_ORDER array]"). Every target above
# already carries its own ordinary install(TARGETS ... RUNTIME DESTINATION
# ${VMS_SYSTEM_DIR}) rule in its own CMakeLists.txt (default "Unspecified"
# component, installed unconditionally by a plain `cmake --install`,
# UNCHANGED by this block). This loop ADDITIONALLY tags each one into a
# second, named install COMPONENT ("ovmx-images") so
# `cmake --install <dir> --component ovmx-images` installs EXACTLY this
# aggregate's members and nothing else -- in particular, none of the OTHER
# targets a plain `cmake --install` would also try to install (vmsssh,
# vms_tcpip_config, vms_sysman, vms_analyze, host factory tooling, tests...),
# which is fatal ("file INSTALL cannot find ...") on any build tree that only
# ran `cmake --build --target ovmx-images` (as tools/cross-vax/build-ovmx-
# images-vax-cmake.sh and tools/cut-release-vax.sh both do) -- those other
# targets were never built, so a plain --install errors trying to copy their
# missing binaries.
#
# This is what lets tools/cut-release-vax.sh derive the shipped VAX
# userspace image set from install_manifest.txt instead of a hand-maintained
# list: add a new target to _OVMX_IMAGES_DEPS above and it is installed by
# this loop automatically, with no second place to remember to edit -- and
# no per-target CMakeLists.txt to touch either, since this loop reads that
# same list.
#
# Library targets (LIBRARY, not RUNTIME) are deliberately skipped -- the
# aggregate's dependency closure pulls shared libs in transitively via
# target_link_libraries, but the SHIPPED "what ships to SYS$SYSTEM" set this
# component models is executables only; libraries keep shipping through
# their own existing SYSLIB install rules, untouched by this rung.
if(BUILD_TOOLS)
    foreach(_ovmx_image_tgt ${_OVMX_IMAGES_DEPS})
        if(TARGET ${_ovmx_image_tgt})
            get_target_property(_ovmx_image_tgt_type ${_ovmx_image_tgt} TYPE)
            if(_ovmx_image_tgt_type STREQUAL "EXECUTABLE")
                install(TARGETS ${_ovmx_image_tgt}
                    RUNTIME DESTINATION ${VMS_SYSTEM_DIR}
                    COMPONENT ovmx-images)
            endif()
        endif()
    endforeach()
endif()

# Install VMS system disk (ODS-2 directory structure)
install(DIRECTORY distro/rootfs/vms/ DESTINATION /vms
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)

# `make release` — convenience wrapper for tools/cut-release.sh (vms-d73,
# epic vms-a84 RELEASE ENGINEERING). The script is the actual machinery
# (git-archives HEAD to a clean tree, runs the containerized build, emits
# dist/release-<version>/ with SHA256SUMS + release-manifest.json) -- this
# target only makes it reachable as `make release` / `ninja release` from an
# already-configured build dir. Not part of ALL; run it explicitly. For
# options (--ref, --out-dir, --no-cache, ...) call tools/cut-release.sh
# directly -- see its --help.
add_custom_target(release
    COMMAND ${CMAKE_SOURCE_DIR}/tools/cut-release.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Cutting an OVMX release bundle via tools/cut-release.sh"
    VERBATIM
    USES_TERMINAL
)
