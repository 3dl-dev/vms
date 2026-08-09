# tests/qemu/CMakeLists.txt
#
# Public sys$ system-service tests for the QEMU kernel-executive harness
# (vms-1d9, epic vms-6b8 Phase 0.5). Unlike tests/qemu/test_kmod_*.c (raw
# ioctl(2) against /dev/vms, compiled separately by tests/qemu/Dockerfile
# with plain gcc -static), these programs link the REAL src/libvms public
# sys$ API and are built through the normal OVMX CMake graph, so that
# reverting production code under src/libvms/syssvc/ is visible to them.
#
# WHY THERE IS NO add_test() HERE -- read before adding one.
# These binaries need a real, insmod'd vms.ko at /dev/vms. `ctest` never
# runs anywhere that has one: not on a dev machine, not in the host build
# job, not in any container (Docker/podman is not the OVMX runtime --
# CLAUDE.md Rule 9). An earlier round of this item DID register them with
# SKIP_RETURN_CODE 77, and the result was an entry that reported "Skipped"
# in 100% of environments where ctest exists, while in the ONE environment
# where the binary can actually pass -- the QEMU initramfs -- ctest is not
# present at all and tests/qemu/init.sh execs the binary directly. So the
# ctest entry never executed as a passing assertion anywhere, ever. Under
# project Rule 10 a permanently-skipped test is a failing test, and an
# honest comment does not discharge that; the fix is to not expose a test
# that cannot run, not to disclose it more loudly.
#
# What actually gates these binaries, and where to look when one fails:
#   * BUILD:  add_executable below is part of the default `all` target, so a
#             test_syssvc_*.c that stops compiling breaks every build,
#             including the plain host CI build job.
#   * RUN:    tests/qemu/init.sh execs every /tests/test_syssvc_* in the
#             initramfs and prints "=== SUITE <name> rc=<code> ===".
#   * ASSERT: .github/workflows/ci.yml (jobs kernel-executive and
#             kernel-executive-negative-control) derives the expected suite
#             set from `ls tests/qemu/test_*.c` and asserts each suite's
#             verdict line individually.
#
# GENERALITY (do not regress this): every test_syssvc_*.c in this directory
# is discovered by glob and built automatically -- there is no per-file list
# to update. That is the fix for a real, adversary-proven defect: the
# harness build/initramfs steps once named exactly ONE binary
# (test_syssvc_lock) by literal string in tests/qemu/Dockerfile, so the next
# test_syssvc_*.c a future item added would build fine locally and then
# silently never enter the initramfs and never run in QEMU CI, with both
# jobs staying green. tests/qemu/Dockerfile now builds the aggregate
# qemu_syssvc_tests target below (not any one test by name) and copies
# build-static/bin/test_syssvc_* by glob.

function(qemu_syssvc_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        ${VMS_INCLUDE_DIR}
        ${VMS_RMS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/src/libvmssys
        # ovmx_image.h / ovmx_symvec.h -- the OVMX symbol-vector image format the
        # in-process resident-import-binding test constructs against (vms-db2).
        # imgact_prodreg.h (a libvms public header) includes ovmx_image.h, so any
        # syssvc test touching the resident registry needs this on its path.
        ${CMAKE_SOURCE_DIR}/src/vmslink/include
    )
    target_link_libraries(${name} PRIVATE
        vms
        vmsrms
        pthread
        m
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

# ---------------------------------------------------------------------------
# USERSPACE-ONLY imgact tests -- test_imgact_bind, test_imgact_publish
# (main-red classification fix; test_imgact_bind was test_syssvc_imgact_bind
# through #225).
#
# .github/workflows/ci.yml's kernel-executive-negative-control job derives its
# "must be a real executive suite" policed set from the test_syssvc_*.c glob
# above and requires every member to return the honest-skip code 77 when
# /dev/vms is absent -- a test_syssvc_* suite that returns 0 with no executive
# is the LARP shape that job exists to catch. These two suites prove pure
# userspace .vms$imp import binding / resident-producer registry publish (see
# their own file headers) -- they never touch /dev/vms and correctly return 0
# in EVERY environment, executive present or not. Naming them test_syssvc_*
# put them under a contract that does not apply to them and made the negative-
# control job flag a correct pass as a fabricated one (main went red on
# test_syssvc_imgact_bind for exactly this reason). test_imgact_* is the
# correct, non-executive name: it is excluded from the test_syssvc_*.c glob
# above (and so from ci.yml's and run_facility_negctl.sh's test_syssvc_*
# policing) automatically, by construction, not by an added exception.
#
# Built through the SAME qemu_syssvc_add_test() plumbing (same libvms link,
# same include path -- test_imgact_bind.c/test_imgact_publish.c #include
# imgact_prodreg.h, a libvms public header) so they still land in
# build-static/bin/ and still run inside the QEMU harness (tests/qemu/
# Dockerfile, tests/qemu/init.sh both glob test_imgact_* alongside
# test_syssvc_*) -- run_facility_negctl.sh's consumer-import-not-bound-to-
# resident / publish-does-not-populate-registry per-facility proofs still
# attribute these suites' FAIL: lines correctly (see that script's own
# EXPECTED derivation, widened to include tests/qemu/test_imgact_*.c).
#
# UNLIKE test_syssvc_*, these two also get a real ctest add_test(): the
# "WHY THERE IS NO add_test() HERE" comment above this file's top applies to
# binaries that NEED a real /dev/vms and so can only ever report ctest
# "Skipped" (a permanently-skipped test is a failing test, project Rule 10).
# test_imgact_bind/test_imgact_publish need no /dev/vms and genuinely pass
# under a bare `ctest` on any host -- exactly the case that comment's
# reasoning does not cover, and exactly what their own file headers already
# claimed ("runs and asserts in EVERY environment, including the CI host
# build") but nothing had actually gated until now.
qemu_syssvc_add_test(test_imgact_bind test_imgact_bind.c)
qemu_syssvc_add_test(test_imgact_publish test_imgact_publish.c)
add_test(NAME test_imgact_bind COMMAND test_imgact_bind)
add_test(NAME test_imgact_publish COMMAND test_imgact_publish)
set_tests_properties(test_imgact_bind test_imgact_publish PROPERTIES
    LABELS "userspace-imgact")

# ---------------------------------------------------------------------------
# TESTIMG.EXE -- the in-process-eligible image test_syssvc_imgact_inproc.c
# activates in DCL's own process (vms-68f increment iv). A freestanding,
# position-independent ET_DYN with the OVMX in-process marker note, no
# PT_INTERP and no symbolic relocations, entered through imgact_activate()'s
# (a0,a1) ABI -- the same freestanding static-PIE shape IMGACT.EXE uses, but
# built in the normal graph (not OVMX_IMGACT mode) so it ships with the static
# musl test build. It is a SUBJECT, not a suite: its name matches no test_*
# glob, so init.sh never runs it as one.
add_executable(testimg_inproc testimg_inproc.c)
target_include_directories(testimg_inproc PRIVATE ${CMAKE_SOURCE_DIR}/src/libvms/include)
target_compile_options(testimg_inproc PRIVATE
    -std=gnu11 -O2 -Wall -Wextra
    -fPIC -ffreestanding -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables)
target_link_options(testimg_inproc PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-e,img_entry -Wl,-z,norelro -Wl,--build-id=none)
set_target_properties(testimg_inproc PROPERTIES
    PREFIX "" OUTPUT_NAME "TESTIMG" SUFFIX ".EXE")

# TESTREAL.EXE -- the REAL-image-class subject test_syssvc_imgact_realimg.c
# activates in-process (vms-db2, the flip). Same freestanding static-PIE build
# as TESTIMG, but entered at real_start through the SysV auxv _start ABI (not the
# (a0,a1) img_entry ABI) and carrying a .vms$imp import table its GOT cells bind
# to the resident producer. Entry symbol is real_start; the link-time-constant
# patch_off (cell - __ehdr_start) means it still builds with zero dynamic relocs.
add_executable(testreal_inproc testreal_inproc.c)
target_include_directories(testreal_inproc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmslink/include)
target_compile_options(testreal_inproc PRIVATE
    -std=gnu11 -O2 -Wall -Wextra
    -fPIC -fno-plt -ffreestanding -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fvisibility=hidden)
target_link_options(testreal_inproc PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-e,real_start -Wl,-z,norelro -Wl,--build-id=none
    # Pin .vms$got so testreal_inproc.c's .vms$imp patch_off literals (0x200000+)
    # name the GOT cells' image-relative vaddrs exactly -- a hand-assembled
    # object cannot compute those as assembly-time constants.
    -Wl,--section-start=.vms$got=0x200000)
set_target_properties(testreal_inproc PROPERTIES
    PREFIX "" OUTPUT_NAME "TESTREAL" SUFFIX ".EXE")

# TESTEXTERN.EXE -- the EXTERNAL-image subject test_syssvc_imgact_extern.c
# activates in-process (vms-db2, the external-image flip). TESTREAL plus a real
# PT_INTERP: an .interp section names the OVMX loader
# (/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE, matching src/vmslink/link.c), which the
# linker wraps in a PT_INTERP program header -- the defining trait of a genuinely
# external LINK.EXE executable. Same freestanding static-PIE build + .vms$got
# pinning as TESTREAL. TESTEXTERN_FOREIGN.EXE is the anti-over-acceptance control:
# identical eligibility but a FOREIGN interp, which must force the fork path.
add_executable(testextern_inproc testextern_inproc.c)
target_include_directories(testextern_inproc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmslink/include)
target_compile_options(testextern_inproc PRIVATE
    -std=gnu11 -O2 -Wall -Wextra
    -fPIC -fno-plt -ffreestanding -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fvisibility=hidden)
target_link_options(testextern_inproc PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-e,extern_start -Wl,-z,norelro -Wl,--build-id=none
    -Wl,--section-start=.vms$got=0x200000)
set_target_properties(testextern_inproc PROPERTIES
    PREFIX "" OUTPUT_NAME "TESTEXTERN" SUFFIX ".EXE")

add_executable(testextern_foreign testextern_foreign.c)
target_include_directories(testextern_foreign PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmslink/include)
target_compile_options(testextern_foreign PRIVATE
    -std=gnu11 -O2 -Wall -Wextra
    -fPIC -fno-plt -ffreestanding -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fvisibility=hidden)
target_link_options(testextern_foreign PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-e,extern_start -Wl,-z,norelro -Wl,--build-id=none)
set_target_properties(testextern_foreign PROPERTIES
    PREFIX "" OUTPUT_NAME "TESTEXTERN_FOREIGN" SUFFIX ".EXE")

# TESTPROD.EXE -- a NON-RESIDENT producer shareable the activator MAPS in-process
# when TESTNONRES.EXE's .vms$imp names it (vms-db2, the non-resident-producer
# sub-step). A pure leaf OVMX symbol-vector shareable: a .vms$sv exporting
# prod_bump (PROCEDURE) + prod_counter (DATA), pinned so its .vms$sv values are
# link-time-constant image-relative vaddrs (0x100000 code / 0x200000 data). No
# entry point (never entered), no PT_TLS/PT_INTERP/PT_DYNAMIC (imgact_map_producer
# accepts exactly this class; anything heavier forks).
add_executable(testprod_shr testprod_shr.c)
target_include_directories(testprod_shr PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmslink/include)
target_compile_options(testprod_shr PRIVATE
    -std=gnu11 -O2 -Wall -Wextra
    -fPIC -fno-plt -ffreestanding -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fvisibility=hidden)
target_link_options(testprod_shr PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-z,norelro -Wl,--build-id=none
    # Pin the producer's code + data so testprod_shr.c's .vms$sv value literals
    # (0x100000 prod_bump, 0x200000 prod_counter) name their image-relative
    # vaddrs exactly -- the same as-time-constant trick the consumer subjects use
    # for .vms$got. ovmx_sv_resolve biases these by the mapped load base.
    -Wl,--section-start=.vms$pcode=0x100000
    -Wl,--section-start=.vms$pdata=0x200000)
set_target_properties(testprod_shr PROPERTIES
    PREFIX "" OUTPUT_NAME "TESTPROD" SUFFIX ".EXE")

# TESTNONRES.EXE -- the consumer whose .vms$imp names the NON-RESIDENT TESTPROD
# (plus the resident LIBVMS$SHR for SYS$EXIT). test_syssvc_imgact_nonres.c
# activates it in-process; the activator maps TESTPROD to bind prod_bump. Same
# freestanding static-PIE build + .vms$got pinning as TESTREAL, entered at
# nonres_start via the SysV auxv _start ABI.
add_executable(testnonres_inproc testnonres_inproc.c)
target_include_directories(testnonres_inproc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmslink/include)
target_compile_options(testnonres_inproc PRIVATE
    -std=gnu11 -O2 -Wall -Wextra
    -fPIC -fno-plt -ffreestanding -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fvisibility=hidden)
target_link_options(testnonres_inproc PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-e,nonres_start -Wl,-z,norelro -Wl,--build-id=none
    -Wl,--section-start=.vms$got=0x200000)
set_target_properties(testnonres_inproc PROPERTIES
    PREFIX "" OUTPUT_NAME "TESTNONRES" SUFFIX ".EXE")

# Single build target covering every registered test_syssvc_* binary, so
# callers (tests/qemu/Dockerfile) never need to name a specific test. It also
# pulls in TESTIMG.EXE so a `--target qemu_syssvc_tests` build stages the
# subject alongside its suite.
get_property(_qemu_syssvc_test_targets GLOBAL PROPERTY QEMU_SYSSVC_TEST_TARGETS)
add_custom_target(qemu_syssvc_tests DEPENDS ${_qemu_syssvc_test_targets} testimg_inproc testreal_inproc testextern_inproc testextern_foreign testprod_shr testnonres_inproc)

# ---------------------------------------------------------------------------
# TORN OUT AS GATES (vms-49f), per the CLAUDE.md Rule 9 amendment.
#
# Five ctest entries used to live here as GATES over the neg-control harness's
# OWN self-declarations -- string relations, not runtime behavior:
#
#   * facility_negctl_manifest   (facility_defects.sh selftest) -- the manifest
#                                is internally consistent: anchors land, named
#                                assertion texts exist literally, the coverage
#                                statement holds.
#   * facility_negctl_record     (facility_record_negctl.sh) -- the execution
#                                LEDGER reader is sound against a SYNTHETIC
#                                record built from the manifest.
#   * facility_attribution_*     (facility_attribution.sh + _negctl) -- joins
#                                the committed ledger with mutation-derived
#                                function names.
#   * facility_negctl_equality   (facility_negctl_equality_negctl.sh) -- the
#                                red-set EXACT-equality's suite scoping.
#
# The Rule 9 amendment: "A check over the tree's own self-declarations (census,
# register, manifest, ledger) proves a string relation, not runtime behavior;
# it may lint but MUST NOT be gated or worded as proof." These re-broke on
# every executive change that shifted which assertions redden and consumed
# enormous effort; a gameable declaration-gate is a tear-out, not a hardening
# target. The property they gestured at -- "a defect is actually caught" -- is
# already covered by the runtime proof: the QEMU per-facility negative-control
# job (.github/workflows/ci.yml) injects each defect, rebuilds vms.ko, boots it
# against a REAL /dev/vms, and asserts the facility's own suite goes RED
# (run_facility_negctl.sh checks 1-4/8). That behavioral proof stays.
#
# The scripts remain in the tree and can be run by hand as INFORMATIONAL LINT
# (`sh tests/qemu/facility_defects.sh selftest .`), but they are no longer
# registered with add_test() and never gate. See docs/design-negctl-degate.md.
# The committed ledger tests/qemu/facility_negctl_observed.tsv was deleted with
# them; run_facility_negctl.sh now treats it as an absent advisory snapshot.

# ---------------------------------------------------------------------------
# THE kernel-executive-negative-control STEP MUST SURVIVE A CRASHING SUITE
# (rd vms-a3c, rework of vms-c9c / commit 74e3200).
#
# vms-c9c's `suite_fails=$(... | grep -F "FAIL:")` in ci.yml only exercises
# the branch where the target suite printed a FAIL: line. A test_syssvc_*
# suite that crashes (segfault, killed by a signal, any exit with NO FAIL:
# line) makes grep exit 1, and because this whole step runs under `bash -e`,
# an unguarded `var=$(cmd)` assignment that fails aborts the step -- with
# ZERO diagnostic, before the per-suite verdicts or the vms.ko/vmsfs.ko
# checks are ever reached. tools/replay_ci_kernel_executive.py extracts that
# step's `run:` block VERBATIM out of ci.yml and executes it against a
# synthetic capture, so this control is exercised against the SAME TEXT CI
# runs -- no QEMU, no container, no /dev/vms needed.
add_test(
    NAME kernel_executive_negctl_crash
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/kernel_executive_negctl_crash.sh
            ${CMAKE_SOURCE_DIR}
)
set_tests_properties(kernel_executive_negctl_crash PROPERTIES LABELS "harness")

# ---------------------------------------------------------------------------
# THE 0.2 DEMO, END TO END (vms-5dd, parent vms-2579).
#
# Boots the ACTUAL mastered bootable image (distro/Dockerfile.bootable),
# logs in as SYSTEM, runs the ACTUAL install procedure a manager would type
# (`$ @SYS$UPDATE:PARTS_SETUP.COM`), then the ACTUAL foreign command
# (`$ PARTS`), and asserts the keyed sys$get lookups are correct, the file
# landed at SYS$SCRATCH:PARTS.DAT (a VMS filespec, not a Unix path), and no
# fallback candidate was ever tried. See tests/qemu/test_parts_demo_e2e.sh's
# header for the full "what this proves that nothing else does" case.
#
# SKIP_RETURN_CODE 77: this needs a real docker + QEMU boot that takes real
# minutes (a blank-disk install, then a real RMS indexed-file load under QEMU
# TCG emulation) and does NOT run under a bare `ctest` -- see
# tests/qemu/run_parts_demo_e2e.sh's header for why an opt-in env var gates
# it rather than SKIP_RETURN_CODE alone: unlike the test_syssvc_* binaries
# above (which truly can never pass under ctest, anywhere), this one CAN pass
# for real wherever docker is available -- it is opt-in to protect existing
# CI job time budgets, not because it is unrunnable.
add_test(
    NAME parts_demo_e2e
    # bash, not /bin/sh: run_parts_demo_e2e.sh uses `set -o pipefail`, a
    # bashism that /bin/sh (dash on Ubuntu) rejects outright ("Illegal
    # option -o pipefail"), which made this ctest entry fail before it
    # ever reached its own SKIP_RETURN_CODE 77 gate -- pre-existing on the
    # branch this fix started from (vms-5dd), never actually run by a
    # bare `ctest`. Same convention as dcl-integration's `COMMAND bash
    # run_dcl_tests.sh` above.
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_parts_demo_e2e.sh
)
set_tests_properties(parts_demo_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 2400)
