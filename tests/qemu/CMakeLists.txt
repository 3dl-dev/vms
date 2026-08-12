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
        # vmsfs_ondisk.h -- test_syssvc_initialize.c (vms-cf62) reads the raw
        # home block off the backing device to prove INITIALIZE wrote it there.
        ${CMAKE_SOURCE_DIR}/src/kernel/vmsfs
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
# DEVICE-CORPUS PROGRAMS UNDER A REAL /dev/vms (vms-08c).
#
# #258 (vms-dv1) correctly converted sys$getdvi/sys$device_scan from a
# per-process host fake to readers of the executive's device table -- and
# that honestly took two corpus programs, lib_getdvi.c and sys_device_scan.c
# (tests/corpus/tier1-examples/, unmodified Eight-Cubed downloads --
# tests/corpus/PROVENANCE.md, never edited or copied), from a FAKE run-pass
# to a run-crash under tests/conformance/run_corpus.sh's HOST harness, which
# has no /dev/vms (Rule 9) and so cannot run either program at all. That is
# an honest floor drop on the host, not a regression: the coverage those two
# programs give still exists, just only where an executive is real.
#
# These two targets build the SAME canonical corpus sources (never a copy --
# provenance forbids editing or duplicating them) with the SAME
# qemu_syssvc_add_test() recipe every real src/libvms-linked suite in this
# file uses, so they run against a genuine /dev/vms inside this QEMU
# harness. Named test_corpus_* (not test_syssvc_*) deliberately: ci.yml's
# kernel-executive-negative-control job polices every test_syssvc_*.c to
# return the honest-skip code 77 with no executive -- a contract this
# project wrote for ITS OWN suites. These two are unmodified upstream
# programs whose no-executive behavior is whatever errchk_sig()/lib$signal()
# actually does (the same run-crash run_corpus.sh already measures on the
# host), not a thing to design an honest-skip contract for -- the same
# reasoning that keeps test_imgact_* out of that glob (see this file's
# comment on that section, below).
qemu_syssvc_add_test(test_corpus_lib_getdvi
    ${CMAKE_SOURCE_DIR}/tests/corpus/tier1-examples/lib_getdvi.c)
qemu_syssvc_add_test(test_corpus_sys_device_scan
    ${CMAKE_SOURCE_DIR}/tests/corpus/tier1-examples/sys_device_scan.c)

# corpus_seed_lnm -- a harness FIXTURE, not a suite (no test_ prefix, so
# tests/qemu/init.sh's suite loop never runs it as one; see its own header
# comment). Built the same way so it shares the exact link graph (and so the
# exact LNM$SYSTEM arena) the two corpus suites above run against.
qemu_syssvc_add_test(corpus_seed_lnm
    ${CMAKE_SOURCE_DIR}/tests/qemu/corpus_seed_lnm.c)

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

# TESTTLS.EXE -- the OWN-PT_TLS REAL-image subject test_syssvc_imgact_tls.c
# activates in-process (vms-db2, the DTV-append / TLS-absorb flip). TESTREAL plus
# its OWN PT_TLS (a .tdata __thread datum -> a PT_TLS program header) reached
# through the OVMX .vms$tls TLSDESC path the activator biases against DCL's thread
# pointer. Same freestanding static-PIE build + .vms$got pinning as TESTREAL, plus
# .vms$tdesc (the writable TLSDESC descriptor the activator fills) pinned so the
# .vms$tls entry names its image-relative offset as a link-time constant.
add_executable(testtls_inproc testtls_inproc.c)
target_include_directories(testtls_inproc PRIVATE
    ${CMAKE_SOURCE_DIR}/src/libvms/include
    ${CMAKE_SOURCE_DIR}/src/vmslink/include)
target_compile_options(testtls_inproc PRIVATE
    -std=gnu11 -O2 -Wall -Wextra
    -fPIC -fno-plt -ffreestanding -fno-stack-protector -fno-builtin
    -fno-asynchronous-unwind-tables -fvisibility=hidden)
target_link_options(testtls_inproc PRIVATE
    -nostdlib -nostartfiles -shared
    -Wl,-e,tls_start -Wl,-z,norelro -Wl,--build-id=none
    -Wl,--section-start=.vms$got=0x200000
    -Wl,--section-start=.vms$tdesc=0x210000)
set_target_properties(testtls_inproc PROPERTIES
    PREFIX "" OUTPUT_NAME "TESTTLS" SUFFIX ".EXE")

# Single build target covering every registered test_syssvc_* binary, so
# callers (tests/qemu/Dockerfile) never need to name a specific test. It also
# pulls in TESTIMG.EXE so a `--target qemu_syssvc_tests` build stages the
# subject alongside its suite.
get_property(_qemu_syssvc_test_targets GLOBAL PROPERTY QEMU_SYSSVC_TEST_TARGETS)
add_custom_target(qemu_syssvc_tests DEPENDS ${_qemu_syssvc_test_targets} testimg_inproc testreal_inproc testextern_inproc testextern_foreign testprod_shr testnonres_inproc testtls_inproc)

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

# ---------------------------------------------------------------------------
# SYSGEN PARAMETER FILE VERSIONING, END TO END (vms-d34).
#
# Boots the ACTUAL mastered bootable image (same distro/Dockerfile.bootable
# as parts_demo_e2e above), logs in as SYSTEM, and proves -- against the
# REAL mounted volume, not a mock -- that WRITE CURRENT creates a genuine
# vmsfs file version (;2 over the seeded ;1) and USE CURRENT reads it back
# at the highest version, across independent SYSGEN.EXE processes. See
# tests/qemu/test_sysgen_versioning_e2e.sh's header for the full sequence
# and what would make it fail honestly.
#
# Same opt-in/SKIP convention as parts_demo_e2e (see run_sysgen_versioning_
# e2e.sh's header): real docker + QEMU boot, real minutes, gated behind
# OVMX_QEMU_FULL_E2E=1 so it does not run under a bare `ctest`.
add_test(
    NAME sysgen_versioning_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_sysgen_versioning_e2e.sh
)
set_tests_properties(sysgen_versioning_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 600)

# ---------------------------------------------------------------------------
# MOUNT/DISMOUNT ARE REAL mount(2)/umount(2) (vms-651).
#
# Boots the ACTUAL mastered bootable image TWICE against a SECOND, real
# virtio disk (formatted VMSFS on the host first -- see run_mount_e2e.sh):
# MOUNT DKA100:, CREATE a real file on it, DISMOUNT, re-MOUNT, then a full
# QEMU process restart against the SAME disk file, proving the file landed
# on the disk and not a tmpfs. See tests/qemu/test_mount_e2e.sh's header for
# the full ground-source shape. Same opt-in convention as parts_demo_e2e
# above, for the same reason (a real docker+QEMU boot, real minutes, must
# not tax a bare `ctest` run's time budget).
add_test(
    NAME mount_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_mount_e2e.sh
)
set_tests_properties(mount_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# ---------------------------------------------------------------------------
# PRODUCT INSTALL onto a real vmsfs volume (vms-df9).
#
# Boots the actual mastered bootable image, MOUNTs a second real virtio disk
# (formatted VMSFS on the host first, same as mount_e2e above), PRODUCT
# INSTALLs the real OS kit already baked into the image (/boot/ovmx-os.kit)
# from a third read-only virtio disk, RUNs the installed HELP.EXE FROM THE
# TARGET device (the anti-LARP crux -- a kit that only wrote a manifest
# cannot pass this), and confirms PRODUCT SHOW PRODUCT plus the installed
# files survive a full QEMU process restart. See
# tests/qemu/test_product_install_e2e.sh's header for the full ground-source
# shape. Same opt-in convention as mount_e2e above, for the same reason.
add_test(
    NAME product_install_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_product_install_e2e.sh
)
set_tests_properties(product_install_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# ---------------------------------------------------------------------------
# PRODUCT INSTALL /DESTINATION -> boot THAT target as its own system disk
# (vms-96ec, parent vms-718, gates the vms-37f R1 release e2e).
#
# product_install_e2e (above) proves a /DESTINATION install lands runnable
# bytes on a mounted DATA volume; it never boots that target. Every booting
# CI e2e boots the PRE-MASTERED ovmx-distrib.img, never a freshly
# /DESTINATION-installed target -- so the on-disk SHAPE a /DESTINATION install
# produces was never booted, which is how vms-649 found it was FLAT
# ([SYSEXE]) instead of the rooted, concealed [SYS0.SYSCOMMON.SYSEXE] a
# bootable system disk needs (a flat target halts %OVMX-F-SYSINIT). This gate
# closes that gap: it installs onto a blank INITIALIZEd disk, then boots THAT
# disk as the sole DKA0: system disk with the bootstrap-only slim initramfs
# and reaches a SYSTEM login -- red on the unfixed tree, green after vms-96ec.
# CI runs it for real (never opt-in cover): see the install-boot-e2e job in
# .github/workflows/ci.yml, which treats a SKIP as a hard failure. Same
# opt-in-in-ctest convention as product_install_e2e above.
add_test(
    NAME install_boot_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_install_boot_e2e.sh
)
set_tests_properties(install_boot_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# ---------------------------------------------------------------------------
# The faithful install menu, OVMX$INSTALL.COM (vms-dcf).
#
# Boots the DISTRIBUTION image straight into the install menu (no login),
# drives it live over the console exactly as an operator would (oracle sec3a:
# INITIALIZE-or-PRESERVE, target device, volume label, the confirmation
# gate), MOUNTs and PRODUCT INSTALLs a real kit onto a real second disk, and
# proves the two-variant SYSTARTUP_VMS.COM staging (the installed target's
# own copy does not re-run the menu). See tests/qemu/test_install_menu.sh's
# header for exactly what is scored vs. captured-but-not-scored (writing the
# password via AUTHORIZE against the target is blocked by prerequisite gaps
# this item found and reported, not fixed here). Same opt-in convention as
# product_install_e2e above, for the same reason.
add_test(
    NAME install_menu_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_install_menu.sh
)
set_tests_properties(install_menu_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# ---------------------------------------------------------------------------
# THE R1 release-acceptance gate: media -> menu install -> SEPARATE container
# -> login -> PRODUCT SHOW PRODUCT (vms-37f, parent vms-718, docs/design-
# vms-faithful-install.md §3.3 "Release e2e").
#
# Unifies the three pieces already on the epic (test_install_menu.sh's menu
# drive, test_install_boot_e2e.sh's boot-the-installed-target, and
# test_docker_persistent_disk.sh's cross-container boundary) into the gate none
# of them cover on its own: the FULL menu-driven install in one container, the
# installed target surviving into a SEPARATE `docker run` that boots it ALONE
# and logs in + PRODUCT SHOW PRODUCT + DIRECTORY, and a third boot proving
# first-boot ran once. Keeps and mechanically ASSERTS the vms-9b7
# writethrough-cache lesson, and its NEGATIVE CONTROL (an install torn down
# before the guest flush, whose verify must then fail) is the red-first proof
# that the durability is real. CI runs it for real (never opt-in cover): the
# release-install-e2e job in .github/workflows/ci.yml treats a SKIP as a hard
# failure. Same opt-in-in-ctest convention as install_menu_e2e above.
add_test(
    NAME release_install_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_release_install.sh
)
set_tests_properties(release_install_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 1800)

# ---------------------------------------------------------------------------
# install -> UPGRADE -> boot (vms-f05, epic vms-a84 RELEASE ENGINEERING).
#
# Cuts two real tools/cut-release.sh bundles (a BASELINE and an UPGRADE,
# different OVMX_PRODUCT_VERSION), PRODUCT INSTALLs the baseline onto a
# blank volume, writes real user state + a site customization, PRODUCT
# INSTALLs the upgrade over the SAME volume, and asserts the user state
# and site customization survive, the version advanced, and the machine
# still boots. Requires the two bundles to be prebuilt -- see
# run_upgrade_e2e.sh's header for the local reproduction recipe and
# .github/workflows/ci.yml's upgrade-e2e job for how CI produces them.
# Same opt-in convention as product_install_e2e above, for the same reason.
add_test(
    NAME upgrade_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_upgrade_e2e.sh
)
set_tests_properties(upgrade_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# ---------------------------------------------------------------------------
# SCSNODE -> REAL HOSTNAME ACROSS REBOOT, END TO END (vms-b6a7).
#
# Boots the ACTUAL mastered bootable image (same distro/Dockerfile.bootable
# as parts_demo_e2e/sysgen_versioning_e2e above) and proves, against the
# REAL mounted volume and a REAL second boot -- not a mock -- that
# STARTUP.EXE's SYSBOOT role reads SYS$SYSTEM:OVMXVMSSYS.PAR and sets the
# actual Linux hostname (sethostname(2), read back via F$GETSYI("NODENAME"))
# from SCSNODE, and that a missing/corrupt parameter file on an already-
# installed disk boots on defaults with an honest OVMX-facility warning
# rather than a fabricated VMS message or a silent halt. See
# tests/qemu/test_boot_scsnode_hostname_e2e.sh's header for the full
# two-case sequence and why F$GETSYI("NODENAME") is the right assertion
# (not F$GETSYI("SCSNODE") or SHOW SYSTEM, both of which already read the
# .PAR file directly and would pass unchanged).
#
# Same opt-in/SKIP convention as the other qemu-full-boot gates: real
# docker + QEMU boot, real minutes, gated behind OVMX_QEMU_FULL_E2E=1 so it
# does not run under a bare `ctest`.
add_test(
    NAME boot_scsnode_hostname_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_boot_scsnode_hostname_e2e.sh
)
set_tests_properties(boot_scsnode_hostname_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# ---------------------------------------------------------------------------
# JOB_CONTROL OWNS THE CONSOLE SESSION, END TO END (vms-8d2).
#
# Boots the ACTUAL mastered bootable image (same distro/Dockerfile.bootable
# as parts_demo_e2e/boot_scsnode_hostname_e2e above), logs in as SYSTEM
# through the login prompt JOB_CONTROL created, and proves via the REAL
# executive process table (`SHOW SYSTEM`, read from a DIFFERENT process than
# the one that created JOB_CONTROL -- CLAUDE.md Rule 11) that JOB_CONTROL is
# a real, named, DETACHED process -- not PID 1 running an inline login loop.
# The cheap, always-on half of this item's proof (PID 1's source carries no
# login loop) is tests/integration/test_job_control_ownership.sh, a source
# scan that runs under a bare `ctest`; this is the expensive runtime half.
# See tests/qemu/test_job_control_console.sh's header for the full
# A-writes/B-reads shape.
#
# Same opt-in/SKIP convention as the other qemu-full-boot gates: real
# docker + QEMU boot, gated behind OVMX_QEMU_FULL_E2E=1 so it does not run
# under a bare `ctest`.
add_test(
    NAME job_control_console_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_job_control_console_e2e.sh
)
set_tests_properties(job_control_console_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 600)

# ---------------------------------------------------------------------------
# JOB_CONTROL COMPONENT-REGISTRATION NEGATIVE CONTROL, END TO END (vms-2a9).
#
# Boots /boot/ovmx-distrib-negctl.img -- a SECOND mastered image
# distro/Dockerfile.bootable builds from the identical staged system tree
# as the normal ovmx-distrib.img, with JOB_CONTROL's CONFIG-phase STDRV
# component registration (SYS$STARTUP:VMS$VMS.DAT) removed and nothing
# else changed -- and proves the boot still runs STARTUP.COM to completion
# but never creates JOB_CONTROL (no %RUN-S-PROC_ID) and never reaches a
# login prompt (JOB_CONTROL is the only thing that creates the console
# login loop, vms-8d2). This is the negative complement to
# job_control_console_e2e above: that one proves the registration, when
# present, produces a real detached process; this one proves the
# registration is what GATES that creation, not just one path among
# several. See tests/qemu/test_job_control_negctl.sh's header for the full
# reasoning, including why this test cannot use the console's own SHOW
# SYSTEM the way the positive test does.
#
# Same opt-in/SKIP convention as the other qemu-full-boot gates: real
# docker + QEMU boot, gated behind OVMX_QEMU_FULL_E2E=1 so it does not run
# under a bare `ctest`.
add_test(
    NAME job_control_negctl_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_job_control_negctl_e2e.sh
)
set_tests_properties(job_control_negctl_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 600)

# ---------------------------------------------------------------------------
# GENERALIZED RELEASE-ACCEPTANCE GATE (vms-a86f, epic vms-a84 RELEASE
# ENGINEERING).
#
# Boots the ACTUAL tools/cut-release.sh (vms-d73) release bundle -- never a
# dev build -- runs the same PARTS 0.2 demo acceptance scenario as
# parts_demo_e2e above, and asserts the artifact's own
# `$ PRODUCT SHOW PRODUCT` reports exactly the OVMX_PRODUCT_VERSION the cut
# recorded in its release-manifest.json (an independent record, not
# re-derived from the artifact under test). Includes an in-transcript
# negative control proving the version-match comparator can actually detect
# a disagreement; see .github/workflows/ci.yml's release-acceptance-e2e job
# for the heavier complementary negative control (rerunning this whole gate
# with a genuinely wrong expected version and asserting the GATE ITSELF
# reddens). Requires a prebuilt bundle -- see run_release_acceptance_e2e.sh's
# header for the local reproduction recipe. Same opt-in convention as
# upgrade_e2e above, for the same reason.
add_test(
    NAME release_acceptance_e2e
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_release_acceptance_e2e.sh
)
set_tests_properties(release_acceptance_e2e PROPERTIES
    LABELS "slow;qemu-full-boot"
    SKIP_RETURN_CODE 77
    TIMEOUT 2400)
