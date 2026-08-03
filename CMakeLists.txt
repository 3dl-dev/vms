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
option(BUILD_TESTS "Build test programs" ON)
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

if(OVMX_STATIC)
    set(OVMX_LIB_TYPE STATIC)
    # Produce fully static executables (no dynamic linker dependency)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
else()
    set(OVMX_LIB_TYPE SHARED)
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

if(BUILD_TESTS)
    add_subdirectory(tests/libvms)
    add_subdirectory(tests/vmsprocess)
    add_subdirectory(tests/vmslnm)
    add_subdirectory(tests/vmsfs)
    add_subdirectory(tests/vmsrms)
    add_subdirectory(tests/vmsqueue)
    add_subdirectory(tests/dcl)
    add_subdirectory(tests/vmsscs)

    # tests/vmsssh: term_mapping unit test only (vms-97d). No libssh
    # dependency — unlike src/vmsssh (the daemon), this must build and run
    # unconditionally so ctest never reports a skip here.
    add_subdirectory(tests/vmsssh)

    # Public sys$ API tests for the QEMU kernel-executive harness (vms-1d9,
    # epic vms-6b8 Phase 0.5). Links real src/libvms against a real
    # /dev/vms when present; SKIP (ctest exit 77), never a fake PASS,
    # when it is not (see tests/qemu/CMakeLists.txt).
    add_subdirectory(tests/qemu)

    # INV-1 standing gate (vms-e652): system identity has ONE owner, and the
    # login banner stays driven by SYS$WELCOME rather than a compiled-in
    # printf. Source scan — no build artifacts needed.
    add_test(NAME identity_ssot_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_identity_ssot.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(identity_ssot_gate PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 60)

    # Rule 9 standing gate: ONE runtime target (kernel/QEMU). Docker is not a
    # runtime, and no executive facility may silently fall back to a
    # per-process userspace fake when /dev/vms is absent. Source scan.
    add_test(NAME runtime_target_gate
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_runtime_target.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(runtime_target_gate PROPERTIES
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

    # ... and the evasions, run against a sandbox copy of the tree. Each must
    # drive the census RED for its own specific reason, and four GREEN controls
    # pin the rules from the other side — including that a function reached only
    # through a LIVE callback table still counts (vms-c13).
    add_test(NAME kif_caller_census_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_kif_caller_census_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    # TIMEOUT raised from 300 (vms-e2b): each control runs a gate that
    # configures cmake and preprocesses every product translation unit.
    # MEASURED 3m29s on workshop at 29 controls; 37 after vms-8cc added the
    # citation controls, 43 after vms-c13 added the reachability ones.
    set_tests_properties(kif_caller_census_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 1800)

    # The citation ledger is fresh (vms-8cc). tracking/rd-citations.tsv is what
    # lets the vms_kif caller census above check that an exemption's cited rd
    # item EXISTS and is OPEN without reaching rd -- rd is nostr-backed and
    # unavailable in CI, which is why that gate shape-checked the id and
    # nothing more until now. (The userspace service register below does NOT
    # read the ledger; wiring it is rd vms-fab. Do not read the census's
    # coverage as the register's.)
    #
    # Committed derived state has one failure mode and it is the whole defect
    # one level up: the ledger goes stale and the gate then certifies a closed
    # item as open. Only this test can see that, and only where rd is
    # reachable, so it EXITS 77 and ctest reports SKIPPED -- never PASSED -- on
    # a host without rd.
    #
    # THE SKIP IS A STANDING RESIDUAL, not a clean result: under project rule 7
    # a skipped test is a failing test, and this one skips on every CI run.
    # What survives the skip is its self-controls and its ledger-FLOOR controls
    # -- 16 of them, all rd-free -- so a skipped run still proves the
    # comparator is not stuck at green and still proves the checker refuses an
    # empty ledger. What does NOT survive is the part that needs rd: the live
    # comparison and the independent read.
    add_test(NAME rd_citations_fresh
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_rd_citations_fresh.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(rd_citations_fresh PROPERTIES
        LABELS "integration;authenticity;needs-rd"
        SKIP_RETURN_CODE 77 TIMEOUT 300)

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

    # ...and the evasions, on a sandbox copy of the tree. The GREEN controls
    # carry as much weight as the RED ones here: the register's design rests on
    # firing for an undeclared impostor and NOT for legitimate pure computation,
    # and on leaving Rule 10's second answer (delete the service outright) open.
    # Each control runs the whole gate, and the gate now compiles the tree for
    # its symbol scan, so the wall clock is (controls x compiles). Measured on
    # an 8-core box: 99s for 34 controls before the symbol scan, 180s for 39
    # after. Re-measured on a 16-core box for vms-c19: 328s for 44 controls
    # before, 387s for 49 after -- the added cmake configure per gate run costs
    # about 0.4s per control, and the control COUNT is what moved. The timeout
    # is set well clear of that because a CI runner has fewer cores and the
    # compiles are what parallelise; it was raised with the control count, not
    # to accommodate a slowdown.
    add_test(NAME userspace_service_register_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_userspace_service_register_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(userspace_service_register_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 1200)

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
    # copy of the tree. A gate nobody tries to evade asserts nothing.
    add_test(NAME terminal_identity_negctl
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_terminal_identity_negctl.sh
                ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(terminal_identity_negctl PROPERTIES
        LABELS "integration;authenticity" TIMEOUT 120)

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
    add_subdirectory(src/vmslink)   # LINK.EXE + OVMXDUMP (VMS-native linker)
    add_subdirectory(src/install)   # INSTALL.EXE (known image database, vms-913.5)
endif()

# Install VMS system disk (ODS-2 directory structure)
install(DIRECTORY distro/rootfs/vms/ DESTINATION /vms
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)
