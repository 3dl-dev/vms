# tests/cluster/host - the R1 host rung for the executive-resident VMScluster
# stack. Everything here builds with a plain host C compiler against
# src/kernel-core ALONE (no src/kernel, no src/kernel-netbsd, no kernel
# headers), so R1 is a sub-second host loop rather than a QEMU boot
# (design docs/design-faithful-cluster-executive.md sec 3.9 test-ladder rung 1).
# `ctest -R cluster_host` selects every test in this directory.
#
# Two groups, from FC-P0.1 (the seam/interface contract) and FC-P0.6 (the codec):
#   - the codec host tests (FC-P0.6): vms_cluster_codec.c + fixture loader.
#   - the pure-header host-includability proof (FC-P0.1): each inter-layer header
#     compiled alone, plus an aggregate smoke.

# ---- FC-P0.6: the wire codec, built with NO kernel headers (plain stdint.h) ----
add_library(cluster_host_codec STATIC
    ${CMAKE_SOURCE_DIR}/src/kernel-core/vms_cluster_codec.c
)
target_include_directories(cluster_host_codec PUBLIC
    ${CMAKE_SOURCE_DIR}/src/kernel-core
)
# No kernel headers, no OVMX_KBACKEND_* define: the plain stdint.h path in
# vms_cluster_codec.h sec "Fixed-width integer vocabulary".

add_library(cluster_host_fixture STATIC
    cluster_fixture.c
    cluster_sha256.c
)
target_include_directories(cluster_host_fixture PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Absolute paths baked in at configure time -- host tests only, never shipped.
set(OVMX_CLUSTER_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set(OVMX_CLUSTER_CLEANROOM_MANIFEST
    "${CMAKE_SOURCE_DIR}/docs/clean-room/reference-captures.sha256")

function(cluster_host_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        cluster_host_codec
        cluster_host_fixture
    )
    target_compile_definitions(${name} PRIVATE
        OVMX_FIXTURE_DIR="${OVMX_CLUSTER_FIXTURE_DIR}"
        OVMX_CLEANROOM_MANIFEST="${OVMX_CLUSTER_CLEANROOM_MANIFEST}"
    )
    add_test(NAME cluster_host_${name} COMMAND ${name})
    set_tests_properties(cluster_host_${name} PROPERTIES
        LABELS "unit;cluster;codec"
        TIMEOUT 30
    )
endfunction()

cluster_host_test(test_codec_wire       test_codec_wire.c)
cluster_host_test(test_codec_classify   test_codec_classify.c)
cluster_host_test(test_codec_allowlist  test_codec_allowlist.c)
cluster_host_test(test_fixture_loader   test_fixture_loader.c)
cluster_host_test(test_codec_roundtrip  test_codec_roundtrip.c)
cluster_host_test(fuzz_codec_seed       fuzz_codec_seed.c)

# ---- FC-P0.1: the pure inter-layer headers must be host-includable ----
# Everything below builds with -DOVMX_CLUSTER_HOST, which routes vms_cluster.h
# to <stdint.h> instead of vms_internal.h.
set(OVMX_CLUSTER_CORE_DIR ${CMAKE_SOURCE_DIR}/src/kernel-core)

# One object per pure header, compiled ALONE (see hdr_alone_*.c's own comment):
# each proves that header needs nothing but the six of them plus
# <stdint.h>/<stddef.h>. OBJECT libraries, not executables -- there is nothing
# to run, only to compile.
add_library(cluster_host_hdr_alone OBJECT
    hdr_alone_vms_cluster.c
    hdr_alone_vms_cluster_snapshot.c
    hdr_alone_vms_pe.c
    hdr_alone_vms_scs.c
    hdr_alone_vms_cnxman.c
    hdr_alone_vms_dlm_scs.c
)
target_include_directories(cluster_host_hdr_alone PRIVATE ${OVMX_CLUSTER_CORE_DIR})
target_compile_definitions(cluster_host_hdr_alone PRIVATE OVMX_CLUSTER_HOST)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cluster_host_hdr_alone PRIVATE -Wall -Wextra)
endif()

# A "did it compile" gate for an OBJECT library: `cmake --build --target <name>`,
# which ctest invokes directly. This is the host-includability proof, registered
# UNCONDITIONALLY: a build failure here must be a RED ctest result.
add_test(NAME cluster_host_headers_alone
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target cluster_host_hdr_alone --config $<CONFIG>
)
set_tests_properties(cluster_host_headers_alone PROPERTIES
    LABELS "unit;cluster;cluster_host"
    TIMEOUT 60
)

# The aggregate integration smoke: all six headers together, in the order a real
# consumer (vms_pe.c) uses, plus a sizeof/enum touch per header.
add_executable(test_headers_host test_headers_host.c)
target_include_directories(test_headers_host PRIVATE ${OVMX_CLUSTER_CORE_DIR})
target_compile_definitions(test_headers_host PRIVATE OVMX_CLUSTER_HOST)
add_dependencies(test_headers_host cluster_host_hdr_alone)

add_test(NAME cluster_host_headers COMMAND test_headers_host)
set_tests_properties(cluster_host_headers PROPERTIES
    LABELS "unit;cluster;cluster_host"
    TIMEOUT 30
)

# ---- FC-P4.9: the HOST backend for vms_lock.c (the real DLM/lock engine) ----
# vms_lock.c compiles UNCHANGED against exec_kbackend.h/exec_list.h/
# exec_hash.h/exec_rbtree.h -- the frozen seam headers are never touched.
# lock_shim/ holds tiny (one #include line) files named EXACTLY what those
# frozen headers' own quoted #includes already ask for
# (exec_kbackend_linux.h, exec_list_linux.h, exec_hash_linux.h,
# exec_rbtree_linux.h, vms_internal.h); putting lock_shim/ FIRST on this
# target's include path redirects those names to the real content
# (exec_*_host.h / lock_host_internal.h, pthreads + malloc + a hand-rolled
# intrusive list/hash/rbtree) without adding src/kernel or src/kernel-netbsd
# to the path at all -- see lock_shim/exec_kbackend_linux.h for the full
# mechanism explanation. src/kernel is still needed, AFTER lock_shim/, for
# the genuinely portable src/kernel/vms_ioctl.h (ioctl arg structs +
# LCK_*/VMS_DLM_*/PSL_C_* constants -- already used from plain userspace by
# every tests/qemu/test_syssvc_*.c program).
find_package(Threads REQUIRED)

add_library(cluster_host_lock STATIC
    ${CMAKE_SOURCE_DIR}/src/kernel-core/vms_lock.c
)
target_include_directories(cluster_host_lock PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_shim
    ${OVMX_CLUSTER_CORE_DIR}
    ${CMAKE_SOURCE_DIR}/src/kernel
)
target_link_libraries(cluster_host_lock PUBLIC Threads::Threads)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cluster_host_lock PRIVATE -Wall -Wextra)
endif()

add_executable(test_lock_host test_lock_host.c)
target_include_directories(test_lock_host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_shim
    ${OVMX_CLUSTER_CORE_DIR}
    ${CMAKE_SOURCE_DIR}/src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(test_lock_host PRIVATE cluster_host_lock Threads::Threads)

add_test(NAME cluster_host_test_lock_host COMMAND test_lock_host)
set_tests_properties(cluster_host_test_lock_host PROPERTIES
    LABELS "unit;cluster;cluster_host;lock"
    TIMEOUT 30
)
