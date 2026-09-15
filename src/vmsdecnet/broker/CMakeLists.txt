# vmsdecnet broker layer - the exec<->NETACP request/response record codec for
# the DECnet _NET: $QIO broker (rd vms-22c, a1-2, transport T1).
#
# dnet_broker: the transport records that ride the executive mailbox (vms_mbx)
# as the network-ACP hop between a user process's $QIO on _NET: (qio_net_op) and
# NETACP's NSP link engine. Pure logic, no socket / no mailbox / no allocation
# (plain stdint + string.h) -- the same discipline as the routing/NSP codecs, so
# it links equally into the executive-side marshaller and the NETACP daemon. The
# two security guards are in the codec itself: every decode is bounds-validated
# (magic gate + datalen bound + total-length check, never over-reads), and a
# monotonically-issued correlation id matches each response to its request
# (anti-cross-talk). Depends on vmsdecnet_nsp only for DNET_NSP_MAX_DATA.
# dnet_ncb: the client-side Network Connect Block parser (rd vms-22c, a1-2) --
# the LOCAL API connect string ($QIO IO$_ACCESS p1, NODE::"TASK=name") qio_net_op
# parses into {node, task/object} before building the SC connect descriptor.
# Pure, bounds-checked, spec-derived (never a wire field). Grouped with the
# broker record as the client-side transport helpers libvms will link.
add_library(vmsdecnet_broker STATIC dnet_broker.c dnet_ncb.c)
target_include_directories(vmsdecnet_broker PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../nsp/include   # dnet_nsp.h (DNET_NSP_MAX_DATA)
)
