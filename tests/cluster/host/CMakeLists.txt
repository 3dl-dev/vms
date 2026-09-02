# tests/cluster/host - the rung-1 host build for the executive-resident
# VMScluster stack (FC-P0.1; design docs/design-faithful-cluster-executive.md
# SS3.9 test ladder rung 1, SS3.2.1/SS3.2.2 the substrate seam).
#
# Everything here builds with a plain host C compiler against
# src/kernel-core ALONE (no src/kernel, no src/kernel-netbsd, no
# exec_kbackend.h) and -DOVMX_CLUSTER_HOST, which routes vms_cluster.h to
# <stdint.h> instead of vms_internal.h. `ctest -R cluster_host` selects every
# test this directory (and, as later FC-P0.x items land the codec and the
# N-node simulator, its siblings) registers.

set(OVMX_CLUSTER_CORE_DIR ${CMAKE_SOURCE_DIR}/src/kernel-core)

# One object per pure header, compiled ALONE (see hdr_alone_*.c's own
# comment): each proves that header needs nothing but the six of them plus
# <stdint.h>/<stddef.h>. OBJECT libraries, not executables -- there is
# nothing to run, only to compile; a link step would need a main() none of
# these six exist to provide.
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
# Freestanding-ish: forbid the compiler from handing this TU a system kernel
# header search path it does not need. -ffreestanding would also drop libc
# prototypes test_headers_host.c wants (printf), so it is not applied to that
# target below -- only to these six, which need nothing past stdint/stddef.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cluster_host_hdr_alone PRIVATE -Wall -Wextra)
endif()

# add_test can only run something with a main(); a "did it compile" gate for
# an OBJECT library is `cmake --build . --target <name>`, which ctest can
# invoke directly. This is the actual host-includability proof (item 3's
# "compiles" requirement) and it is registered UNCONDITIONALLY, like every
# other structural gate in this tree: a build failure here must be a RED
# ctest result, not a configure-time surprise nobody runs.
add_test(NAME cluster_host_headers_alone
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target cluster_host_hdr_alone --config $<CONFIG>
)
set_tests_properties(cluster_host_headers_alone PROPERTIES
    LABELS "unit;cluster;cluster_host"
    TIMEOUT 60
)

# The aggregate integration smoke: all six headers together, the order a real
# consumer (vms_pe.c) uses, plus a sizeof/enum touch per header so an empty
# guard-only header would fail here too. This one DOES run (item 3's "and
# compiles" plus a trivial pass/fail signal beyond "the build succeeded").
add_executable(test_headers_host test_headers_host.c)
target_include_directories(test_headers_host PRIVATE ${OVMX_CLUSTER_CORE_DIR})
target_compile_definitions(test_headers_host PRIVATE OVMX_CLUSTER_HOST)
# Depend on the alone-objects target too, so `ctest -R cluster_host` at
# minimum BUILDS everything in this directory even if a caller runs only this
# one test by name.
add_dependencies(test_headers_host cluster_host_hdr_alone)

add_test(NAME cluster_host_headers COMMAND test_headers_host)
set_tests_properties(cluster_host_headers PROPERTIES
    LABELS "unit;cluster;cluster_host"
    TIMEOUT 30
)
