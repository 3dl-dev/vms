# vmsdecnet DAP layer - DECnet Phase IV DAP (Data Access Protocol) message codec
# (rd vms-8c2, epic vms-30e; north-star demo leg vms-e4dc), the presentation
# layer behind $ COPY node::file and the FAL server (object 17).
#
# dnet_dap: the DAP message codec -- encode/decode of the CONFIGURATION,
# ATTRIBUTES, ACCESS, CONTROL, DATA, STATUS, NAME, ACCESS-COMPLETE message set
# the oracle's real VAX<->VAX COPY exercised. Like the NSP and CTERM codecs this
# is a PURE byte library: no socket, no allocation, no clock -- so it links
# equally into the daemon, the FAL server and the deterministic unit test.
#
# CLEAN-ROOM (Rule 8): the message TYPES + generic framing are the PUBLIC DEC
# DAP functional specification; the SEQUENCE and the carried values (filename,
# resolved full spec, owner UIC, verbatim records) are the ground truth of
# docs/oracle/vax-copy-fal-dap.* (rd vms-cd3). The per-field sub-framing is
# spec-derived and self-round-tripping, NOT asserted byte-identical to the
# real-VAX DAP sub-framing (oracle §3) -- byte-level VAX FAL interop is a filed
# follow-on. See dnet_dap.h for the full provenance note.
add_library(vmsdecnet_dap STATIC dnet_dap.c)
target_include_directories(vmsdecnet_dap PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
