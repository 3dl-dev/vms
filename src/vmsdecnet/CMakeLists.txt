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
