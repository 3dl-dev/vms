# Dockerfile for the OVMX/NetBSD amd64 harness (rd vms-7f7, epic vms-8e8).
#
# Carries qemu-system-x86_64 + anita + the NetBSD serial-console tooling, so the
# whole "boot NetBSD/amd64 and assert uname" proof runs inside one container --
# nothing is installed on the host. This is the NetBSD-substrate sibling of
# tests/qemu/Dockerfile (which does the same for the Linux vms.ko executive).
#
# This is BUILD/TEST TOOLING, not a runtime (CLAUDE.md Rule 9 /
# docs/runtime-target.md): it boots a real NetBSD guest in QEMU to TEST it,
# exactly as tests/qemu/ boots a real Linux guest. NetBSD-as-a-real-runtime is
# the product goal of epic vms-8e8; a QEMU harness that boots it for testing is
# tooling.
#
# Usage:
#   docker build -f tests/netbsd/Dockerfile -t ovmx-netbsd-ktest .
#   docker run --rm -v "$PWD/.netbsd-cache:/cache" \
#              --device /dev/kvm ovmx-netbsd-ktest      # --device optional
#
# Exit 0 = NetBSD booted and `uname -srm` asserted; nonzero = failure/timeout.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# qemu-system-x86  : the amd64 emulator anita drives
# qemu-utils       : qemu-img (disk image handling)
# python3/pip      : anita is a Python program
# python3-pexpect  : anita "screen scrapes" the serial console via pexpect
# genisoimage      : anita builds the install-sets ISO with mkisofs/genisoimage
# curl/ca-certs    : anita downloads the pinned release from cdn.netbsd.org
# gzip, cpio       : anita unpacks release media
RUN apt-get update && apt-get install -y --no-install-recommends \
        qemu-system-x86 \
        qemu-utils \
        python3 \
        python3-pip \
        python3-pexpect \
        genisoimage \
        curl \
        ca-certificates \
        gzip \
        cpio \
        gcc \
        libc6-dev \
    && rm -rf /var/lib/apt/lists/*

# anita is pinned (see tests/netbsd/netbsd_version.env). It is NOT the unrelated
# PyPI package named "anita"; the real tool ships as a tarball from gson.org, so
# we download that, verify its SHA256, and pip-install it. Ubuntu 24.04 marks the
# system Python environment externally-managed; this is a throwaway test image
# with no other Python software to disturb, so installing into it is fine.
ARG ANITA_VERSION=2.18
ARG ANITA_SHA256=fa5ac3a8b3e30a3a35df9254bbee213b28210a396544592c3e604a1e53a294ce
RUN curl -sSL -o /tmp/anita.tar.gz \
        "https://www.gson.org/netbsd/anita/download/anita-${ANITA_VERSION}.tar.gz" \
    && echo "${ANITA_SHA256}  /tmp/anita.tar.gz" | sha256sum -c - \
    && pip3 install --no-cache-dir --break-system-packages /tmp/anita.tar.gz \
    && rm -f /tmp/anita.tar.gz \
    && anita --version

COPY tests/netbsd/netbsd_version.env /netbsd/netbsd_version.env
COPY tests/netbsd/drive_netbsd.py    /netbsd/drive_netbsd.py
COPY tests/netbsd/run_tests.sh       /netbsd/run_tests.sh
# The deterministic console driver (rd vms-2d9), imported by ALL three harnesses
# (drive_netbsd.py, drive_netbsd_p2b.py, drive_netbsd_p2c.py). It sits in /netbsd
# next to the drivers, so `import netbsd_console' resolves via the script dir.
COPY tests/netbsd/netbsd_console.py  /netbsd/netbsd_console.py

# P2b (rd vms-bfe) + P2c (rd vms-4b4): the OVMX/NetBSD `vms' pseudo-device, the
# transport seam leaf, and the userspace probes -- staged for the in-guest build.
# The drivers bundle /netbsd/guest-src into an ISO, attach it to the boot, and
# build inside the NetBSD guest. Selected at run time by overriding the
# entrypoint (/netbsd/run_p2b.sh or /netbsd/run_p2c.sh) on the same image.
#
# THE MODULE NOW COMPILES THE SHARED EXECUTIVE CORE. Since P2c, the `vms' module
# builds src/kernel-core/vms_eflag.c (the SAME event-flag facility source the
# Linux vms.ko builds) with the NetBSD backend. So the guest source tree carries
# THREE dirs: kmod/ (the NetBSD backend glue + headers), kernel-core/ (the shared
# facility + shim contracts), and probe/ (the userspace tools + transport). The
# Makefile in kmod/ reaches ../kernel-core for the shared source. P2b builds the
# same module (it just exercises only PING); both drivers stage kernel-core.
#
# --- kmod/: the NetBSD backend glue + its headers ---
COPY src/kernel-netbsd/vms_netbsd.c          /netbsd/guest-src/kmod/vms_netbsd.c
COPY src/kernel-netbsd/Makefile              /netbsd/guest-src/kmod/Makefile
COPY src/kernel-netbsd/vms_ping.h            /netbsd/guest-src/kmod/vms_ping.h
COPY src/kernel-netbsd/vms_eflag_nb.h        /netbsd/guest-src/kmod/vms_eflag_nb.h
COPY src/kernel-netbsd/vms_internal.h        /netbsd/guest-src/kmod/vms_internal.h
COPY src/kernel-netbsd/exec_kbackend_netbsd.h /netbsd/guest-src/kmod/exec_kbackend_netbsd.h
COPY src/kernel-netbsd/exec_list_netbsd.h    /netbsd/guest-src/kmod/exec_list_netbsd.h
COPY src/kernel-netbsd/exec_list_netbsd.c    /netbsd/guest-src/kmod/exec_list_netbsd.c
# P4-A (rd vms-f8a): the LAST FOUR facilities' wire-contract headers + the
# hash/rbtree backends src/kernel-netbsd/Makefile's SRCS (and
# tests/netbsd/crosscompile.sh, the per-PR build-only gate) already require --
# vms_internal.h #includes all four, and vms_netbsd.c dispatches their ioctls,
# but the in-guest QEMU build never staged them, so the P2c-era harness could
# only ever build the event-flag facility in-guest. This closes that gap: the
# SAME set crosscompile.sh cross-compiles per PR now also builds+boots+links
# in the QEMU harness.
COPY src/kernel-netbsd/vms_ast_nb.h          /netbsd/guest-src/kmod/vms_ast_nb.h
COPY src/kernel-netbsd/vms_access_nb.h       /netbsd/guest-src/kmod/vms_access_nb.h
COPY src/kernel-netbsd/vms_mbx_nb.h          /netbsd/guest-src/kmod/vms_mbx_nb.h
COPY src/kernel-netbsd/vms_acp_nb.h          /netbsd/guest-src/kmod/vms_acp_nb.h
COPY src/kernel-netbsd/vms_proctab_nb.h      /netbsd/guest-src/kmod/vms_proctab_nb.h
COPY src/kernel-netbsd/vms_lock_nb.h         /netbsd/guest-src/kmod/vms_lock_nb.h
COPY src/kernel-netbsd/exec_hash_netbsd.h    /netbsd/guest-src/kmod/exec_hash_netbsd.h
COPY src/kernel-netbsd/exec_hash_netbsd.c    /netbsd/guest-src/kmod/exec_hash_netbsd.c
COPY src/kernel-netbsd/exec_rbtree_netbsd.h  /netbsd/guest-src/kmod/exec_rbtree_netbsd.h
COPY src/kernel-netbsd/exec_rbtree_netbsd.c  /netbsd/guest-src/kmod/exec_rbtree_netbsd.c
# Logical-name facility (rd vms-72da): the wire-contract twin vms_internal.h
# #includes, and the uvm arena/mmap glue TU src/kernel-netbsd/Makefile's SRCS
# lists. Without these the in-guest `make' fails at `vms_lnm_nb.h: No such file'
# (vms_internal.h:81) and cannot link vms_lnm_{init,cleanup}/vms_ioctl_lnm_*/vms_mmap.
COPY src/kernel-netbsd/vms_lnm_nb.h          /netbsd/guest-src/kmod/vms_lnm_nb.h
COPY src/kernel-netbsd/vms_lnm_arena_netbsd.c /netbsd/guest-src/kmod/vms_lnm_arena_netbsd.c
COPY src/kernel-netbsd/vms_acct_rss_netbsd.c /netbsd/guest-src/kmod/vms_acct_rss_netbsd.c
COPY src/kernel-netbsd/vms_sysmem_netbsd.c   /netbsd/guest-src/kmod/vms_sysmem_netbsd.c
# --- kernel-core/: the shared, substrate-agnostic facility + shim contracts ---
COPY src/kernel-core/vms_eflag.c             /netbsd/guest-src/kernel-core/vms_eflag.c
COPY src/kernel-core/exec_kbackend.h         /netbsd/guest-src/kernel-core/exec_kbackend.h
COPY src/kernel-core/exec_list.h             /netbsd/guest-src/kernel-core/exec_list.h
# P4-A: the remaining five shared facility sources (rd vms-f8a) -- SAME sources
# the Linux vms.ko and the per-PR crosscompile.sh gate build; no NetBSD copy of
# any facility's logic exists.
COPY src/kernel-core/vms_ast.c               /netbsd/guest-src/kernel-core/vms_ast.c
COPY src/kernel-core/vms_access.c            /netbsd/guest-src/kernel-core/vms_access.c
COPY src/kernel-core/vms_mbx.c               /netbsd/guest-src/kernel-core/vms_mbx.c
COPY src/kernel-core/vms_proctab.c           /netbsd/guest-src/kernel-core/vms_proctab.c
COPY src/kernel-core/vms_lock.c              /netbsd/guest-src/kernel-core/vms_lock.c
# Logical-name facility source (rd vms-72da) -- the SAME shared source the Linux
# vms.ko and the per-PR crosscompile.sh gate build; listed in the Makefile SRCS.
COPY src/kernel-core/vms_lnm.c               /netbsd/guest-src/kernel-core/vms_lnm.c
# Device-table facility source (rd vms-618) -- the executive-resident device
# table behind $ALLOC/$DALLOC/$ASSIGN/$DASSGN/$GETDVI/$DEVICE_SCAN, the SAME
# shared source the Linux vms.ko builds; listed in the Makefile SRCS, so the
# in-guest module build needs it staged (the four-enumeration rule).
COPY src/kernel-core/vms_devtab.c            /netbsd/guest-src/kernel-core/vms_devtab.c
COPY src/kernel-netbsd/vms_devtab_nb.h       /netbsd/guest-src/kmod/vms_devtab_nb.h
COPY src/kernel-core/exec_hash.h             /netbsd/guest-src/kernel-core/exec_hash.h
COPY src/kernel-core/exec_rbtree.h           /netbsd/guest-src/kernel-core/exec_rbtree.h
# --- Files-11 ODS-2 ACP (vms-d5d): the ACP handlers + the NetBSD block-device
# seam + the genuine ODS-2 codec (ods2_reader/edit, built under -DOVMX_ODS2_KERNEL).
# These joined the module SRCS (src/kernel-netbsd/Makefile) with the ACP flip, so
# the in-guest module build needs them staged. ods2_reader.c/ods2_edit.c live in
# src/vmsfs/ods2 (Makefile .PATH ../vmsfs/ods2) and include "ods2_kcompat.h" +
# "vmsfs/ods2.h" (-I../vmsfs/include). ---
COPY src/kernel-core/vmsfs_acp.c             /netbsd/guest-src/kernel-core/vmsfs_acp.c
COPY src/kernel-netbsd/vms_blockdev_netbsd.c /netbsd/guest-src/kmod/vms_blockdev_netbsd.c
COPY src/kernel-netbsd/vms_socket_netbsd.c   /netbsd/guest-src/kmod/vms_socket_netbsd.c
COPY src/vmsfs/ods2/ods2_reader.c            /netbsd/guest-src/vmsfs/ods2/ods2_reader.c
COPY src/vmsfs/ods2/ods2_edit.c              /netbsd/guest-src/vmsfs/ods2/ods2_edit.c
COPY src/vmsfs/ods2/ods2_kcompat.h           /netbsd/guest-src/vmsfs/ods2/ods2_kcompat.h
COPY src/vmsfs/ods2/ods2_block.h             /netbsd/guest-src/vmsfs/ods2/ods2_block.h
COPY src/vmsfs/include/vmsfs/ods2.h          /netbsd/guest-src/vmsfs/include/vmsfs/ods2.h
# --- FC-P0.1: the executive-resident VMScluster stack's LAN port layer + the
# NetBSD half of the cluster seam (exec_kbackend.h families SS14..SS18). vms_pe.c
# is in src/kernel-netbsd/Makefile's SRCS from its first commit, so the in-guest
# module build needs it and its kernel-core headers staged. ---
COPY src/kernel-core/vms_pe.c                /netbsd/guest-src/kernel-core/vms_pe.c
COPY src/kernel-core/vms_pe.h                /netbsd/guest-src/kernel-core/vms_pe.h
COPY src/kernel-core/vms_scs.h               /netbsd/guest-src/kernel-core/vms_scs.h
COPY src/kernel-core/vms_cnxman.h            /netbsd/guest-src/kernel-core/vms_cnxman.h
COPY src/kernel-core/vms_dlm_scs.h           /netbsd/guest-src/kernel-core/vms_dlm_scs.h
COPY src/kernel-core/vms_cluster.h           /netbsd/guest-src/kernel-core/vms_cluster.h
COPY src/kernel-core/vms_cluster_snapshot.h  /netbsd/guest-src/kernel-core/vms_cluster_snapshot.h
# --- FC-P3.6: the CLUB/CSB model + the CSB ten-state ladder, and the
# RECNXINTERVAL/TIMVCFAIL reconnect loop. Both are in SRCS from their first
# commit for the same reason vms_pe.c is, so the in-guest module build needs
# them staged. ---
COPY src/kernel-core/vms_cnxman_csb.c        /netbsd/guest-src/kernel-core/vms_cnxman_csb.c
COPY src/kernel-core/vms_cnxman_csb.h        /netbsd/guest-src/kernel-core/vms_cnxman_csb.h
COPY src/kernel-core/vms_cnxman_recnx_fsm.c  /netbsd/guest-src/kernel-core/vms_cnxman_recnx_fsm.c
COPY src/kernel-core/vms_cnxman_recnx_fsm.h  /netbsd/guest-src/kernel-core/vms_cnxman_recnx_fsm.h
# --- FC-P3.7: quorum arithmetic (tracking only) + the $GETSYI cluster
# item-code projection. Same reason as FC-P3.6 above: both are in SRCS from
# their first commit, so the in-guest module build needs them staged. ---
COPY src/kernel-core/vms_cnxman_quorum.c     /netbsd/guest-src/kernel-core/vms_cnxman_quorum.c
COPY src/kernel-core/vms_cnxman_quorum.h     /netbsd/guest-src/kernel-core/vms_cnxman_quorum.h
COPY src/kernel-core/vms_cluster_api.c       /netbsd/guest-src/kernel-core/vms_cluster_api.c
COPY src/kernel-core/vms_cluster_api.h       /netbsd/guest-src/kernel-core/vms_cluster_api.h
# --- FC-P0.8: the NISCA channel state machine and the codec it builds every
# frame through (FC-P0.6 shared + FC-P0.7 HELLO/SOLICIT). All three are in SRCS
# from this commit for the same reason vms_pe.c is, so the in-guest module build
# needs them staged. ---
COPY src/kernel-core/vms_pe_fsm.c            /netbsd/guest-src/kernel-core/vms_pe_fsm.c
COPY src/kernel-core/vms_pe_fsm.h            /netbsd/guest-src/kernel-core/vms_pe_fsm.h
COPY src/kernel-core/vms_wire_types.h        /netbsd/guest-src/kernel-core/vms_wire_types.h
COPY src/kernel-core/vms_cluster_codec.c     /netbsd/guest-src/kernel-core/vms_cluster_codec.c
COPY src/kernel-core/vms_cluster_codec.h     /netbsd/guest-src/kernel-core/vms_cluster_codec.h
COPY src/kernel-core/vms_cluster_codec_hello.c /netbsd/guest-src/kernel-core/vms_cluster_codec_hello.c
COPY src/kernel-core/vms_cluster_codec_hello.h /netbsd/guest-src/kernel-core/vms_cluster_codec_hello.h
# --- FC-P3.1 / FC-P3.5 / FC-P3.12: the VMS$VAXcluster SYSAP codec, the
# state-transition barrier's PARTICIPANT side, the p. 7-42 Phase 2 commit both
# halves share, and the COORDINATOR side. All are already in the NetBSD
# module's SRCS (src/kernel-netbsd/Makefile) -- the codec and the barrier were
# listed there without being staged here, which tests/netbsd/check_guest_payload.sh
# reports; these lines close that and keep the new pair from repeating it. ---
COPY src/kernel-core/vms_cluster_codec_cm.c  /netbsd/guest-src/kernel-core/vms_cluster_codec_cm.c
COPY src/kernel-core/vms_cluster_codec_cm.h  /netbsd/guest-src/kernel-core/vms_cluster_codec_cm.h
COPY src/kernel-core/vms_cnxman_phase2.c     /netbsd/guest-src/kernel-core/vms_cnxman_phase2.c
COPY src/kernel-core/vms_cnxman_phase2.h     /netbsd/guest-src/kernel-core/vms_cnxman_phase2.h
COPY src/kernel-core/vms_cnxman_barrier_fsm.c /netbsd/guest-src/kernel-core/vms_cnxman_barrier_fsm.c
COPY src/kernel-core/vms_cnxman_barrier_fsm.h /netbsd/guest-src/kernel-core/vms_cnxman_barrier_fsm.h
COPY src/kernel-core/vms_cnxman_coord_fsm.c  /netbsd/guest-src/kernel-core/vms_cnxman_coord_fsm.c
COPY src/kernel-core/vms_cnxman_coord_fsm.h  /netbsd/guest-src/kernel-core/vms_cnxman_coord_fsm.h
COPY src/kernel-netbsd/vms_lan_netbsd.c      /netbsd/guest-src/kmod/vms_lan_netbsd.c
# FC-P0.4: cluster_seam.c (the R3 substrate-contract self-test for the cluster
# seam, called from vms_netbsd.c's MODULE_CMD_INIT) lives under
# tests/netbsd/guest/ in the repo but is staged INTO kmod/ (not probe/) here,
# alongside the Makefile it builds with -- check_guest_payload.sh's resolve()
# only checks kernel-netbsd/ and kernel-core/ (the src/vmsfs/ods2 precedent,
# docs memory netbsd-module-srcs-four-places), so this staging line is NOT
# gate-verified and must be kept in sync by hand.
COPY tests/netbsd/guest/cluster_seam.c       /netbsd/guest-src/kmod/cluster_seam.c
# --- FC-P0.5: the cluster FORK CONTEXT. vms_cluster_fork.c is the pure queue/
# dispatch half (no substrate type at all) and vms_cluster_fork_bind.c binds its
# ops to exec_kthread_*/exec_timer_*/exec_mutex_*; both are in the NetBSD
# module's SRCS from their first commit, so the in-guest build needs them. ---
COPY src/kernel-core/vms_cluster_fork.c      /netbsd/guest-src/kernel-core/vms_cluster_fork.c
COPY src/kernel-core/vms_cluster_fork.h      /netbsd/guest-src/kernel-core/vms_cluster_fork.h
COPY src/kernel-core/vms_cluster_fork_bind.c /netbsd/guest-src/kernel-core/vms_cluster_fork_bind.c
# --- probe/: the userspace tools + the NetBSD transport seam ---
COPY tests/netbsd/guest/vmsprobe.c           /netbsd/guest-src/probe/vmsprobe.c
COPY tests/netbsd/guest/vmseflag.c           /netbsd/guest-src/probe/vmseflag.c
COPY src/libvmssys/kif_transport_netbsd.c    /netbsd/guest-src/probe/kif_transport_netbsd.c
COPY src/libvmssys/kif_transport.h           /netbsd/guest-src/probe/kif_transport.h
COPY src/kernel-netbsd/vms_ping.h            /netbsd/guest-src/probe/vms_ping.h
COPY src/kernel-netbsd/vms_eflag_nb.h        /netbsd/guest-src/probe/vms_eflag_nb.h
# P4-A (rd vms-f8a): the cross-process test tools for the remaining five
# facilities (proctab, mbx, lock, access, ast) + the wire-contract headers they
# #include.
COPY tests/netbsd/guest/vmsproctab.c         /netbsd/guest-src/probe/vmsproctab.c
COPY tests/netbsd/guest/vmsmbx.c             /netbsd/guest-src/probe/vmsmbx.c
COPY tests/netbsd/guest/vmslock.c            /netbsd/guest-src/probe/vmslock.c
COPY tests/netbsd/guest/vmsaccess.c          /netbsd/guest-src/probe/vmsaccess.c
COPY src/kernel-netbsd/vms_proctab_nb.h      /netbsd/guest-src/probe/vms_proctab_nb.h
COPY src/kernel-netbsd/vms_mbx_nb.h          /netbsd/guest-src/probe/vms_mbx_nb.h
COPY src/kernel-netbsd/vms_lock_nb.h         /netbsd/guest-src/probe/vms_lock_nb.h
COPY src/kernel-netbsd/vms_ast_nb.h          /netbsd/guest-src/probe/vms_ast_nb.h
COPY src/kernel-netbsd/vms_access_nb.h       /netbsd/guest-src/probe/vms_access_nb.h
COPY tests/netbsd/drive_netbsd_p2b.py        /netbsd/drive_netbsd_p2b.py
COPY tests/netbsd/run_p2b.sh                 /netbsd/run_p2b.sh
COPY tests/netbsd/drive_netbsd_p2c.py        /netbsd/drive_netbsd_p2c.py
COPY tests/netbsd/run_p2c.sh                 /netbsd/run_p2c.sh
COPY tests/netbsd/drive_netbsd_p4a.py        /netbsd/drive_netbsd_p4a.py
COPY tests/netbsd/run_p4a.sh                 /netbsd/run_p4a.sh

# P4-VFS V4 (rd vms-308) — RETIRED by vms-165. This was the OVMX/NetBSD ODS-2
# vnode/VFS-backend mount+read proof: it staged the NetBSD ODS-2 vnode backend
# (vmsfs_vfsops.c), the shared substrate-neutral ODS-2 core, vmsfs_ondisk.h and
# an in-guest mount(2) helper, plus a host-mastered ODS-2 volume, and mounted it
# in-guest. The whole vmsfs VFS driver (NetBSD vmsfs.kmod + Linux vmsfs.ko + the
# shared kernel-core/vmsfs core + their host helpers guest-vmsfs/, vmsfs-master/,
# run_vmsfs.sh, drive_netbsd_vmsfs.py, mkimage_vmsfs) was deleted -- the runtime
# reads SYS$DISK over the executive Files-11 ACP, never a vmsfs mount.

RUN chmod +x /netbsd/run_tests.sh /netbsd/drive_netbsd.py \
             /netbsd/run_p2b.sh /netbsd/drive_netbsd_p2b.py \
             /netbsd/run_p2c.sh /netbsd/drive_netbsd_p2c.py \
             /netbsd/run_p4a.sh /netbsd/drive_netbsd_p4a.py

# The installed NetBSD image + download mirror live under /cache so they persist
# across runs when a cache volume is mounted there (the cold-install cost is
# paid once). Nothing here bakes the multi-hundred-MB image into a layer.
VOLUME ["/cache"]

ENTRYPOINT ["/netbsd/run_tests.sh"]
