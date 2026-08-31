#!/bin/bash
# labjoin_lib.sh - pure, sourceable logic for the booted-OVMX-joins-a-real-VAX
# acceptance harness (rd vms-fa1a, parent vms-110b; enables vms-5a23).
#
# WHY A SEPARATE LIB. The heavy half of the harness (labjoin_booted.sh) can only
# run against lab-2 (a genuine OpenVMS VAX V7.3 cluster in a k3s pod) with a
# booted OVMX node emulated under TCG inside the pod -- a precious, slow, external
# resource. But the DECISIONS the harness makes -- is this identity safe to mint,
# is the pod actually a 2-node cluster, do the QEMU tap args match the shipped
# launcher's contract, and above all DID THE JOIN ACTUALLY HAPPEN (the verdict) --
# are pure functions of text. Factoring them here lets the CI plumbing test and
# the negctl exercise the EXACT verdict/guard code the real gate uses, with no
# k3s, no VAX and no boot. That is what keeps the acceptance verdict honest: the
# same lj_verdict() that grades the real lab-2 run is proven to have teeth in CI.
#
# Nothing here talks to kubectl, QEMU or a pod. It is text in, verdict out.

# ---------------------------------------------------------------------------
# Identity guard. SCSNODE and SCSSYSTEMID are cluster-wide unique keys; present
# one the peer's config poller has recently seen on ANOTHER system and the join
# is refused outright ("%PEA0, Remote System Conflicts with Known System"), which
# looks EXACTLY like the vms-2f3 rejoin stall and files a null result against the
# wrong cause (tests/lab/README.md "Mint every identity through mk_sysgen.py").
#
# mk_sysgen.py --alloc already avoids every id in the shared registry, but lab-2
# pods REUSE the same VAX SCSSYSTEMIDs (1025/1026/1027) per pod BY DESIGN (each
# pod is an isolated netns), and those per-pod ids are NOT necessarily in the
# host registry. So we additionally, explicitly, refuse the pod's reserved VAX
# ids here -- belt and suspenders against the one collision the coordinator's
# lab intel called out by name.
# ---------------------------------------------------------------------------
LJ_RESERVED_IDS="${LJ_RESERVED_IDS:-1025 1026 1027}"

lj_guard_identity() {  # <scssystemid>  -> 0 ok, 1 reserved/invalid (msg on stderr)
    local sysid="$1" r
    case "$sysid" in
        ''|*[!0-9]*)
            echo "labjoin: FATAL -- SCSSYSTEMID '$sysid' is not a positive integer" >&2
            return 1 ;;
    esac
    if [ "$sysid" -lt 1 ] || [ "$sysid" -ge 65536 ]; then
        echo "labjoin: FATAL -- SCSSYSTEMID $sysid out of range (1..65535)" >&2
        return 1
    fi
    for r in $LJ_RESERVED_IDS; do
        if [ "$sysid" = "$r" ]; then
            echo "labjoin: FATAL -- SCSSYSTEMID $sysid collides with a lab-2 pod VAX id" \
                 "($LJ_RESERVED_IDS) -- a collision is a false REMOTE-NODE stall that mimics" \
                 "the vms-2f3 bug. Mint a fresh pair: mk_sysgen.py --alloc <prefix> <registry>." >&2
            return 1
        fi
    done
    return 0
}

# ---------------------------------------------------------------------------
# QEMU tap netdev args for the booted OVMX node, bridged to the pod's br0. This
# MIRRORS the shipped launcher distro/boot/run-qemu.sh OVMX_NET_MODE=tap contract
# exactly (see that file's `tap)` case) -- the plumbing test asserts the two are
# byte-identical against run-qemu.sh's own OVMX_QEMU_DRYRUN output, so this can
# never silently drift from the real launcher.
# ---------------------------------------------------------------------------
lj_tap_netdev_args() {  # <tap-ifname> <mac>  -> prints the -netdev/-device tokens
    local tap="$1" mac="$2"
    printf '%s\n' \
        "-netdev" "tap,id=net0,ifname=${tap},script=no,downscript=no" \
        "-device" "virtio-net-pci,netdev=net0,romfile=,mac=${mac}"
}

# Strip NULs + ANSI escapes from a raw VMS console log (from lab2run.sh clean()).
lj_clean() {  # <file>  -> cleaned text on stdout
    tr -d '\000' < "$1" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
}

# Extract the last CN_<n> token a WRITE SYS$OUTPUT "CN_"+F$STRING(F$GETSYI(...))
# probe emitted on a VAX console. Prints the integer, or nothing.
lj_parse_cn() {  # reads console text on stdin -> prints N (or empty)
    grep -aoE 'CN_[0-9]+' | tail -1 | sed 's/^CN_//'
}

# ---------------------------------------------------------------------------
# THE VERDICT. This is the anti-fabrication instrument: it is the ONLY thing that
# says the milestone is met. A booted OVMX node has joined a REAL VAX cluster iff
# ALL FOUR are true, and it must never pass on fewer:
#
#   (a) OVMX side sees the VAX   : the booted node's DCL SHOW CLUSTER (which reads
#       the EXECUTIVE membership block, vms-551 -- not a file) renders a VAX
#       member, and does NOT report %SYSTEM-W-NOSUCHDEV (executive absent) or a
#       lone NOTMEMBER.
#   (b) VAX side sees the OVMX   : vax1's SHOW CLUSTER lists the authored OVMX
#       SCSNODE. This is the genuine VMS cluster's own view -- the clean-room
#       oracle vouching that the join is real, not an OVMX-side self-report.
#   (c) the cluster grew        : vax1's F$GETSYI("CLUSTER_NODES") == 3 (a lab-2
#       pod is 2 VAXes; a real join makes it 3).
#   (d) it happened on the wire : the join pcap is non-empty and carries the OVMX
#       identity in the 0x6007 LAVC/SCA traffic (strings match, mirroring
#       lab2run.sh's "identity on the wire" guardrail).
#
# Any missing leg => FAIL, with the reason printed. Returns 0 only on PASS.
# ---------------------------------------------------------------------------
lj_verdict() {  # <ovmx_node> <ovmx_showcluster_txt> <vax_showcluster_txt> <cn> <pcap>
    local node="$1" ovmx_sc="$2" vax_sc="$3" cn="$4" pcap="$5"
    local ok=1
    local a=0 b=0 c=0 d=0
    node="$(printf '%s' "$node" | tr '[:lower:]' '[:upper:]' | cut -c1-6)"

    # (a) OVMX side sees a VAX member, executive present.
    if grep -qaiE 'NOSUCHDEV' <<<"$ovmx_sc"; then
        echo "  verdict: (a) FAIL -- OVMX SHOW CLUSTER reported NOSUCHDEV: the executive/SCS is"
        echo "                       not up (expected pre-vms-5ad: booted node does not auto-start SCS yet)"
    elif grep -qaiE 'VAX[0-9]' <<<"$ovmx_sc"; then
        a=1; echo "  verdict: (a) PASS -- OVMX SHOW CLUSTER lists a VAX member (executive membership)"
    else
        echo "  verdict: (a) FAIL -- OVMX SHOW CLUSTER shows no VAX member (not joined from the OVMX side)"
    fi

    # (b) VAX side (the oracle) lists the OVMX node.
    if grep -qaF "$node" <<<"$vax_sc"; then
        b=1; echo "  verdict: (b) PASS -- vax1 SHOW CLUSTER lists the OVMX node $node (the VAX oracle sees it)"
    else
        echo "  verdict: (b) FAIL -- vax1 SHOW CLUSTER does not list $node (the real VAX never admitted it)"
    fi

    # (c) cluster grew to 3.
    if [ "$cn" = "3" ]; then
        c=1; echo "  verdict: (c) PASS -- vax1 CLUSTER_NODES=3 (2 VAXes + 1 OVMX)"
    else
        echo "  verdict: (c) FAIL -- vax1 CLUSTER_NODES=${cn:-?} (want 3; still the bare 2-VAX pod)"
    fi

    # (d) the join is on the wire under the OVMX identity.
    if [ -s "$pcap" ] && strings -a "$pcap" 2>/dev/null | grep -qaF "$node"; then
        d=1; echo "  verdict: (d) PASS -- join pcap carries $node in the 0x6007 SCA traffic"
    else
        echo "  verdict: (d) FAIL -- join pcap empty or missing $node (no captured handshake on the wire)"
    fi

    [ "$a" = 1 ] && [ "$b" = 1 ] && [ "$c" = 1 ] && [ "$d" = 1 ] && ok=0
    if [ "$ok" = 0 ]; then
        echo "  BOOTED-OVMX CLUSTER JOIN: PASS -- a booted OVMX node joined a real VAX VMScluster."
    else
        echo "  BOOTED-OVMX CLUSTER JOIN: FAIL -- not all four legs proven (see reasons above)."
    fi
    return "$ok"
}
