# vmsdecnet - DECnet Phase IV for OVMX (rd epic vms-30e).
#
# Clean-room VMS-faithful DECnet Phase IV layered product. See
# docs/design-decnet-ovmx.md for the architecture and
# docs/decnet-provenance-register.md for the Rule-8 provenance.
#
# Rung 1 (rd vms-851) lands the routing-layer Ethernet Endnode Hello codec
# -- a pure, engine-agnostic byte-layout library validated against the vms-3be
# lab-oracle specimen. Rung 2 (rd vms-6986) adds the NSP transport codec,
# round-tripped against the vms-3be NSP Connect Initiate specimen. The remaining
# rungs (adjacency state machine, the AF_DECnet forward-port vs userspace-NSP
# engine go/no-go, and the L3-L6 VMS surface) are tracked as children of
# vms-851 / vms-30e.
add_subdirectory(routing)
add_subdirectory(nsp)

# NCP configuration surface (rd vms-1e9): the persisted node database + the
# NCP.EXE Network Control Program (SET/DEFINE/SHOW/CLEAR NODE, SET/SHOW
# EXECUTOR) -- the "configuration" pillar + the node NAME<->address resolution
# the SET HOST / NODE:: paths resolve through.
add_subdirectory(ncp)

# CTERM layer (rd vms-4d2, rung 3): the Command Terminal protocol behind
# $ SET HOST -- the terminal-service layered product that rides on an
# established NSP logical link (nsp/dnet_link). Pure codec + session FSM,
# entirely SPEC-DERIVED (no oracle specimen; register sec 4.7).
add_subdirectory(cterm)

# DAP layer (rd vms-8c2): the Data Access Protocol message codec -- the
# presentation layer behind $ COPY node::file, riding an NSP logical link. Pure
# codec (message set + generic framing from the public DAP spec; sequence +
# carried values from docs/oracle/vax-copy-fal-dap.*, rd vms-cd3).
add_subdirectory(dap)

# FAL layer (rd vms-8c2): the File Access Listener (Session Control object 17)
# server + the COPY node:: client. Authenticates the connect-carried credentials
# (real SYSUAF/Purdy) and moves the file through RMS over the ODS-2 ACP.
add_subdirectory(fal)

# Engine rung 1 (rd vms-449d): the userspace routing ENGINE that MOVES FRAMES
# using the engine-agnostic codecs above -- AF_PACKET SOCK_RAW over the shared
# src/libdatalink raw-L2 datalink (operator ruling vms-a1c, Option B), with the
# socket hidden behind a VMS-faithful DECnet surface (Rule 1). The daemon
# (DECNETD.EXE) is the DECnet analogue of scsd.
add_subdirectory(engine)
