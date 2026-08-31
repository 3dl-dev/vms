# vmsdecnet routing layer - DECnet Phase IV routing-layer codecs (rd vms-851).
#
# dnet_hello: Ethernet Endnode Hello encode/decode. Pure byte-layout, no
# socket / no allocation / no dependency on any other OVMX library (plain
# stdint + string.h memcpy/memset), so it links equally into the in-kernel
# net/decnet validation harness or the design sec-4b userspace fallback
# engine -- whichever the vms-851 go/no-go settles on.
# dnet_adjacency (rd vms-b15, rung 3): a pure, clock-injectable adjacency state
# machine + hello-cadence/listen-timer engine over dnet_hello. Same engine-
# agnostic, no-socket/no-thread/no-alloc discipline as the codec.
add_library(vmsdecnet_routing STATIC dnet_hello.c dnet_adjacency.c)
target_include_directories(vmsdecnet_routing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
