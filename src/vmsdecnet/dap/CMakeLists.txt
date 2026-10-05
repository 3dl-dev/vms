# vmsdecnet DAP layer - DECnet Phase IV DAP (Data Access Protocol) message codec
# (rd vms-8c2, re-grounded rd vms-a8a, epic vms-30e; north-star demo leg vms-e4dc), the presentation
# layer behind $ COPY node::file and the FAL server (object 17).
#
# dnet_dap: the DAP message codec -- encode/decode of the CONFIGURATION,
# ATTRIBUTES, ACCESS, CONTROL, DATA, STATUS, NAME, ACCESS-COMPLETE message set
# the oracle's real VAX<->VAX COPY exercised. Like the NSP and CTERM codecs this
# is a PURE byte library: no socket, no allocation, no clock -- so it links
# equally into the daemon, the FAL server and the deterministic unit test.
#
# CLEAN-ROOM (Rule 8): field types + message layouts are the PUBLIC DEC DAP 5.6
# specification (AA-K177A-TK); every real OpenVMS DAP buffer in the oracle
# (docs/oracle/vax-copy-fal-dap.*) and the rd vms-a8a lab captures
# (tests/lab/captures/decnet-fal-dap-20261004/) decodes field by field, and the
# encoder's output was accepted by a real VMS FAL. See dnet_dap.h.
add_library(vmsdecnet_dap STATIC dnet_dap.c)
target_include_directories(vmsdecnet_dap PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
