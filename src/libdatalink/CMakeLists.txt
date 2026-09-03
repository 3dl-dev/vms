# src/libdatalink - the raw-L2 Ethernet datalink abstraction (rd vms-838).
#
# RELOCATED HERE BY FC-P3.9. This library was `vmsscs_datalink`, built inside
# src/vmsscs for SCSD, the userspace SCS cluster daemon. That daemon is
# RETIRED with the rest of the userspace SCS strawman (the operator's
# 2026-09-02 clustering reset): the cluster port, SCS and the connection
# manager are executive-resident, so nothing on the cluster path opens a raw
# socket from userspace any more.
#
# The library survives its original caller because it was deliberately written
# engine-agnostic for a SECOND consumer that now exists: DECnet Phase IV
# (src/vmsdecnet/engine/decnetd.c, rd vms-30e). Deleting it with the daemon
# would have taken DECnet's transport with it -- which is exactly the "verify
# no live code dependency first" check the retirement required.
#
# Builds unconditionally, with no dependency on the OVMX library chain: it is
# plain POSIX (Linux AF_PACKET) / NetBSD bpf(4), the same footing it had
# before.
add_library(ovmx_datalink STATIC ovmx_datalink.c)
target_include_directories(ovmx_datalink PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# NOTE ON THE EXECUTIVE BACKEND. src/vmsscs carried an
# OVMX_SCS_DATALINK_EXECUTIVE option that compiled the SCS wire's raw L2
# through /dev/vms (VMS_IOCTL_L2_*) so a non-root SCSD needed no CAP_NET_RAW.
# It is NOT carried over: its only consumer was SCSD, and the cluster wire it
# protected is now inside the executive by construction -- vms_pe.c opens the
# datalink through exec_l2_open() in kernel context, so there is no userspace
# raw socket on that path to gate. DECnet's own executive-side transport is
# rd vms-30e's business, not this library's.
