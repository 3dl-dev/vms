# tests/boundary_audit -- unit proof for the executive-boundary AUDIT tracer
# (vms-c08). Real seccomp user-notif listener, raw syscalls, no mock of the
# tracer (anti-LARP: the instrument's own test is real). Exits 77 (ctest SKIP,
# with a reason) only when the kernel refuses an unprivileged user-notif
# listener -- never a fake PASS.

add_executable(test_boundary_audit test_boundary_audit.c)

target_link_libraries(test_boundary_audit PRIVATE boundary_audit)

add_test(NAME boundary_audit_unit COMMAND test_boundary_audit)
set_tests_properties(boundary_audit_unit PROPERTIES
    LABELS "unit;authenticity;executive-boundary"
    SKIP_RETURN_CODE 77
    TIMEOUT 60
)
