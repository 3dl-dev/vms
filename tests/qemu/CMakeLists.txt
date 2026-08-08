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

# Single build target covering every registered test_syssvc_* binary, so
# callers (tests/qemu/Dockerfile) never need to name a specific test.
get_property(_qemu_syssvc_test_targets GLOBAL PROPERTY QEMU_SYSSVC_TEST_TARGETS)
add_custom_target(qemu_syssvc_tests DEPENDS ${_qemu_syssvc_test_targets})

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
