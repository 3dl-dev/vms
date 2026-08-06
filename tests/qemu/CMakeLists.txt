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
        ${CMAKE_SOURCE_DIR}/src/libvmssys
    )
    target_link_libraries(${name} PRIVATE
        vms
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

# Single build target covering every registered test_syssvc_* binary, so
# callers (tests/qemu/Dockerfile) never need to name a specific test.
get_property(_qemu_syssvc_test_targets GLOBAL PROPERTY QEMU_SYSSVC_TEST_TARGETS)
add_custom_target(qemu_syssvc_tests DEPENDS ${_qemu_syssvc_test_targets})

# ---------------------------------------------------------------------------
# The negative-control manifest's OWN self-test (vms-e7d).
#
# This one IS registered with add_test(), and the paragraph above does not
# apply to it: it needs no /dev/vms, no vms.ko and no container. It copies
# src/kernel and src/libvmssys to a temporary tree, applies each recorded
# facility defect to the copy, and checks two things that can only be checked
# against the CURRENT source:
#
#   * every mutation still finds its sed anchor -- a manifest that has drifted
#     from the tree would inject NOTHING, and tests/qemu/run_facility_negctl.sh
#     would then report the executive gate as having caught a defect that was
#     never applied ("BROKEN FIXTURE, not a broken gate");
#   * re-applying a mutation is detected as a no-op, which proves the
#     injection-landed check fires rather than being a cmp that always passes;
#   * every assertion text the manifest names exists LITERALLY in a suite
#     source. The QEMU driver now requires the observed red set to EQUAL the
#     named set, so a typo'd entry can never be observed and would fail the
#     control blaming the executive for a manifest defect;
#   * the coverage statement holds at every granularity -- every src/kernel/*.c
#     has a control; every derived tests/qemu suite is either reachable by one
#     or declared out of scope with a reason (being declared blind no longer
#     counts as coverage -- vms-279); every defect is ANCHORED by a
#     /* negctl: ... */ comment at the assertion it names, and every anchor
#     names a live defect; and every defect has exactly two case arms here.
#     The out-of-scope set (vmsfs) is printed, so this gate's PASS can never be
#     read as a claim about the whole harness.
#
#     The anchor check is the floor under the SIZE of the manifest, and it is
#     why this ctest matters more than it used to: MEASURED on the checkout
#     that closed vms-b33, 37 of the 42 entries could be deleted one at a time
#     with this test still green, and an exact set cover deleted 32 of 42 in
#     one edit. Both are now caught. What is still possible, stated rather than
#     claimed closed, is in facility_defects.sh's header.
#
# It runs in seconds in the ordinary Build & Test job, so anchor drift is
# caught there instead of twenty minutes into the QEMU job -- or, worse, not
# at all, because a dead anchor makes the QEMU control go GREEN.
add_test(
    NAME facility_negctl_manifest
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/facility_defects.sh
            selftest ${CMAKE_SOURCE_DIR}
)
set_tests_properties(facility_negctl_manifest PROPERTIES LABELS "harness")

# ---------------------------------------------------------------------------
# The negative controls ON the execution-sourced attribution record
# (rd vms-d894, rd vms-659).
#
# tests/qemu/facility_negctl_record.sh is what lets the STATIC gates say
# anything execution-sourced at all: run_facility_negctl.sh emits a record of
# what it observed, and `coverage` derives its observed count floor and its
# PROVEN-able-to-go-red suite population from that record instead of from the
# manifest's own declarations about itself.
#
# A reader like that has exactly one way to be wrong that matters: certifying a
# record that contradicts the tree, which would turn "declared" back into
# "PROVEN" on no evidence. So it gets its own controls, in a sandbox, against a
# SYNTHETIC record built from the manifest -- no QEMU, no container, no
# /dev/vms, and nothing it produces is evidence about the executive. It runs in
# the ordinary Build & Test job in well under a minute.
add_test(
    NAME facility_negctl_record
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/facility_record_negctl.sh
            ${CMAKE_SOURCE_DIR}
)
set_tests_properties(facility_negctl_record PROPERTIES LABELS "harness")

# ---------------------------------------------------------------------------
# PER-ASSERTION RUNTIME ATTRIBUTION, and its own negative controls
# (rd vms-38c, rd vms-d33, rd vms-2b2).
#
# tests/qemu/facility_attribution.sh joins WHAT EACH INJECTION REALLY MUTATES
# (re-derived here, by running the real `facility_defects.sh apply` against a
# throwaway tree and reading the enclosing function of every line the sed
# actually changed) with WHAT REALLY WENT RED (the committed execution record).
# The result is the one statement no static reader in this tree can make:
#
#     mutating <function> was OBSERVED to change the verdict of <assertion>.
#
# BOTH tests below run WITHOUT docker, QEMU or /dev/vms, in seconds, in the
# ordinary Build & Test job -- deliberately. The instrument's whole value is
# that it survives the buys that beat the static gates, and a control that only
# runs in the twenty-minute QEMU job is a control nobody sees fail.
#
# facility_attribution_negctl.sh CONSTRUCTS THE 2-EDIT BUY rd vms-38c recorded
# (flip sys$wflor's OVMX-PARTIAL+OVMX-LOCAL block to OVMX-EXECUTIVE, plus one
# ignored call placed AFTER `return`), runs the OLD register against it -- which
# must still exit 0, or the control is stale -- and then runs the measured check,
# which must refuse it. It also diffs the attribution rows before and after the
# ignored call to show it bought nothing. It invokes the register, so it is the
# slower of the two; still no container.
add_test(
    NAME facility_attribution_selftest
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/facility_attribution.sh selftest
)
set_tests_properties(facility_attribution_selftest PROPERTIES
    LABELS "harness"
    ENVIRONMENT "FA_REPO_ROOT=${CMAKE_SOURCE_DIR}")

add_test(
    NAME facility_attribution_negctl
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/facility_attribution_negctl.sh
)
set_tests_properties(facility_attribution_negctl PROPERTIES
    LABELS "harness"
    TIMEOUT 2400)
