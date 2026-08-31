# vmsdecnet routing layer - DECnet Phase IV routing-layer codecs (rd vms-851).
#
# dnet_hello: Ethernet Endnode Hello encode/decode. Pure byte-layout, no
# socket / no allocation / no dependency on any other OVMX library (plain
# stdint + string.h memcpy/memset), so it links equally into the in-kernel
# net/decnet validation harness or the design sec-4b userspace fallback
# engine -- whichever the vms-851 go/no-go settles on.
add_library(vmsdecnet_routing STATIC dnet_hello.c)
target_include_directories(vmsdecnet_routing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
