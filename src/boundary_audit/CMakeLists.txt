# src/boundary_audit -- Executive-boundary AUDIT tracer (vms-c08, Phase A).
#
# A HOSTED library (pthreads + seccomp user-notif), NOT the freestanding
# libvmssys layer: the supervisor needs a real thread and libc. It is the
# observe-only instrument under vms-040 -- it records raw VMS-semantic syscalls
# an OVMX image issues instead of routing through the executive (/dev/vms).
#
# See docs/design-executive-boundary-audit-tracer.md.

add_library(boundary_audit STATIC
    boundary_audit.c
)

target_include_directories(boundary_audit PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(boundary_audit PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    -Wall -Wextra
)

# pthread for the supervisor thread.
find_package(Threads REQUIRED)
target_link_libraries(boundary_audit PUBLIC Threads::Threads)

set(BOUNDARY_AUDIT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    CACHE INTERNAL "boundary_audit public headers")
