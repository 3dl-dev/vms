# vmsdecnet NSP layer - DECnet Phase IV NSP transport codec (rd vms-6986).
#
# dnet_nsp: NSP transport-message encode/decode (Connect Initiate/Confirm, Data
# segment, Data Acknowledgement, Disconnect Initiate). Pure byte-layout, no
# socket / no allocation / no dependency on any other OVMX library (plain
# stdint + string.h memcpy/memset), so -- exactly like the rung-1 routing codec
# -- it links equally into the forward-ported in-kernel net/decnet validation
# harness or the design sec-4b userspace fallback engine. Round-trips the
# vms-3be lab-oracle NSP Connect Initiate specimen (register sec 4.6, #3).
add_library(vmsdecnet_nsp STATIC dnet_nsp.c)
target_include_directories(vmsdecnet_nsp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
