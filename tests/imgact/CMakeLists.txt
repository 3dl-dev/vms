# tests/imgact -- executive-boundary AUDIT tracer INTEGRATION proof (vms-617).
#
# Drives a REAL activated OVMX image through the REAL IMGACT.EXE activation
# site with the freestanding raw-clone supervisor armed behind
# OVMX_BOUNDARY_AUDIT=1. Proves done-conditions 1 (positive), 3 (transparency),
# and 4 (negctl) against a genuine seccomp user-notif listener -- no mock of the
# tracer (this is an anti-LARP instrument, so its own test is real).
#
# The harness builds its own IMGACT.EXE + test image off-tree (in a temp dir),
# so it needs no OVMX_IMGACT CMake mode and never mutates the checkout. It exits
# 77 (ctest SKIP, with a reason) when the kernel refuses an unprivileged
# user-notif listener, or on a non-x86_64 build arch (Phase A is x86_64) --
# never a fake PASS (Rule 9 / INV-6).

add_test(NAME boundary_audit_imgact_e2e
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_boundary_audit_e2e.sh)
set_tests_properties(boundary_audit_imgact_e2e PROPERTIES
    LABELS "integration;authenticity;executive-boundary;imgact"
    SKIP_RETURN_CODE 77
    TIMEOUT 180)
