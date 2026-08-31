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
    # The cluster LAN logical address is aa:00:04:00:<LE16(SCSSYSTEMID)>, i.e. a
    # DECnet Phase IV address = area*1024 + node. An SCSSYSTEMID that is a multiple
    # of 1024 maps to node 0, an ILLEGAL DECnet node number: a real VAX drops a
    # HELLO whose source logical address is node 0, so it never solicits the joiner
    # and the join sequencer stays JS_IDLE. pcap-proven on lab-2 (vms-a84d): OVMXJ0
    # minted at 1024 (= area 1, node 0) got zero directed solicit from vaxlab-2.
    if [ $(( sysid % 1024 )) -eq 0 ]; then
        echo "labjoin: FATAL -- SCSSYSTEMID $sysid maps to DECnet node 0 (area $(( sysid / 1024 ))," \
             "node 0), an illegal cluster LAN logical address (aa:00:04:00:...). A real VAX won't" \
             "solicit a node-0 joiner -- pick an id whose low 10 bits are non-zero, e.g. $(( sysid + 4 ))." >&2
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

# ---------------------------------------------------------------------------
# CAP_NET_RAW DENIAL -- the anti-fabrication TEETH (rd vms-fa1a, leg (e)).
#
# THE LIE THIS KILLS. 0.6 was sold as the cluster milestone on a probe that never
# was a booted node: a bare SCSD.EXE ELF run IN THE POD, opening an AF_PACKET raw
# socket on br0 directly -- and it only worked because the k3s pod's root process
# carries CAP_NET_RAW in its ambient/default container cap set. That ambient cap
# was the crutch: the "join" rode the pod's caps, not the OVMX executive.
#
# THE FAITHFUL FIX (peer, vms-7eb + piece-2). The executive owns the L2 datalink
# as a KERNEL socket (sock_create_kern(AF_PACKET,SOCK_RAW), src/kernel-core/
# vms_l2.c) -- a kernel socket is NOT subject to any userspace process's caps --
# and the booted SCSD.EXE is compiled with its direct-AF_PACKET path #ifdef'd
# out. So a booted node's join goes through the executive, needing ZERO Linux
# caps in userspace.
#
# THE ASSERTION. Run the whole booted-OVMX node process subtree (the in-pod QEMU
# and everything under it) with CAP_NET_RAW DROPPED from its capability set. Then:
#   - a real join via the executive kernel socket still works (kernel unaffected);
#   - a probe-style userspace direct AF_PACKET raw socket gets EPERM.
# A GREEN gate with the cap denied is therefore PROOF the executive did the I/O,
# not an ambient cap. If the cap were still present, the crutch could be back and
# the gate must FLAG it (that is leg (e), fail-closed).
#
# CAP_NET_RAW is capability number 13; its bit in a CapEff/CapBnd mask is 1<<13.
# ---------------------------------------------------------------------------
LJ_CAP_NET_RAW_BIT="${LJ_CAP_NET_RAW_BIT:-13}"

# The privilege-dropping argv PREFIX for launching the booted-OVMX node process
# with CAP_NET_RAW stripped from every capability set (effective, permitted,
# bounding) of the process AND its whole descendant subtree. Prepended to the
# QEMU command by labjoin_pod_boot.sh. capsh --drop removes cap_net_raw from the
# bounding set AND clears it from the effective/permitted sets of the launched
# process (verified: a root container's a80435fb -> a80415fb, net_raw bit gone),
# so no process in the subtree can hold it -- a userspace AF_PACKET SOCK_RAW open
# then EPERMs, while the executive's KERNEL socket is unaffected. CAP_NET_ADMIN
# (the tap the guest NIC rides) is retained. `-- -c 'exec "$@"' _` execs the
# argv that follows the prefix, array-safe (no re-quoting of the QEMU args).
lj_netraw_deny_argv() {  # -> prints the capsh drop-prefix tokens, one per line
    printf '%s\n' \
        "capsh" "--drop=cap_net_raw" "--" "-c" 'exec "$@"' "_"
}

# Does a CapEff/CapBnd hex mask have CAP_NET_RAW set? Input: the bare hex digits
# (e.g. 00000000a80435fb). Return 0 (true) if net_raw is PRESENT, 1 if CLEAR/
# unparseable. The gate uses this to REFUSE a run whose booted-node context still
# had the crutch.
lj_mask_has_netraw() {  # <hexmask>  -> 0 if net_raw present, 1 if clear/bad
    local hex="$1"
    hex="${hex#0x}"
    case "$hex" in ''|*[!0-9a-fA-F]*) return 1 ;; esac
    # bash arithmetic is 64-bit; CapEff is a 64-bit mask.
    if [ $(( (16#$hex >> LJ_CAP_NET_RAW_BIT) & 1 )) -eq 1 ]; then
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# LEG (e) VERDICT: was CAP_NET_RAW denied to the booted-OVMX node's process
# context during the run? Reads the capability EVIDENCE recorded by
# labjoin_pod_boot.sh -- the booted-node process's /proc/<pid>/status Cap* lines,
# captured live after launch. PASS iff the evidence is present AND both CapEff and
# CapBnd have net_raw CLEAR. FAIL-CLOSED: missing/empty/unparseable evidence, or
# net_raw present in either set, FAILS. A gate cannot prove the executive did the
# I/O if it cannot even show the crutch was absent.
# ---------------------------------------------------------------------------
lj_cap_denied_verdict() {  # <cap-evidence-text>  -> 0 PASS, 1 FAIL (reason on stdout)
    local evid="$1"
    local eff bnd
    if [ -z "${evid//[[:space:]]/}" ]; then
        echo "  verdict: (e) FAIL -- no CAP_NET_RAW evidence recorded for the booted node"
        echo "                       (fail-closed: cannot prove the ambient-cap crutch was absent)"
        return 1
    fi
    eff="$(printf '%s\n' "$evid" | grep -aoiE 'CapEff:[[:space:]]*[0-9a-fA-F]+' | head -1 | grep -aoE '[0-9a-fA-F]+$')"
    bnd="$(printf '%s\n' "$evid" | grep -aoiE 'CapBnd:[[:space:]]*[0-9a-fA-F]+' | head -1 | grep -aoE '[0-9a-fA-F]+$')"
    if [ -z "$eff" ] || [ -z "$bnd" ]; then
        echo "  verdict: (e) FAIL -- CAP_NET_RAW evidence has no CapEff/CapBnd mask (unparseable)"
        return 1
    fi
    if lj_mask_has_netraw "$eff"; then
        echo "  verdict: (e) FAIL -- booted node ran WITH CAP_NET_RAW in effective set (CapEff=$eff):"
        echo "                       the ambient-cap crutch was present -- a join here is NOT proof the"
        echo "                       executive did the I/O. Drop CAP_NET_RAW from the booted-OVMX run."
        return 1
    fi
    if lj_mask_has_netraw "$bnd"; then
        echo "  verdict: (e) FAIL -- CAP_NET_RAW present in the booted node's bounding set (CapBnd=$bnd):"
        echo "                       a descendant could regain it. Drop it from the whole subtree."
        return 1
    fi
    echo "  verdict: (e) PASS -- CAP_NET_RAW denied to the booted node (CapEff=$eff CapBnd=$bnd, net_raw clear):"
    echo "                       a userspace AF_PACKET raw open would EPERM, so a join proves the executive did it"
    return 0
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

# ---------------------------------------------------------------------------
# THE FULL 0.6 GATE VERDICT = the four-leg join (lj_verdict) AND the cap-denied
# teeth (lj_cap_denied_verdict). This is what labjoin_booted.sh grades. The join
# proves membership happened; the cap-denied leg proves it happened WITHOUT the
# ambient-CAP_NET_RAW crutch that the shipped LARP relied on -- i.e. that the
# executive kernel socket did the L2 I/O. BOTH are required. Either one failing
# fails the gate. That is the anti-fabrication contract: a green here cannot be
# the crutch, because the crutch was provably denied.
# ---------------------------------------------------------------------------
lj_booted_gate_verdict() {  # <node> <ovmx_sc> <vax_sc> <cn> <pcap> <cap-evidence>
    local node="$1" ovmx_sc="$2" vax_sc="$3" cn="$4" pcap="$5" cap_evid="$6"
    local join=1 cap=1

    lj_verdict "$node" "$ovmx_sc" "$vax_sc" "$cn" "$pcap"; join=$?
    echo "  --- anti-fabrication teeth: was CAP_NET_RAW denied to the booted node? ---"
    lj_cap_denied_verdict "$cap_evid"; cap=$?

    if [ "$join" = 0 ] && [ "$cap" = 0 ]; then
        echo "  BOOTED-OVMX CLUSTER JOIN (cap-denied): PASS -- joined a real VAX cluster with"
        echo "  CAP_NET_RAW dropped: the OVMX executive did the L2 I/O, not an ambient Linux cap."
        return 0
    fi
    echo "  BOOTED-OVMX CLUSTER JOIN (cap-denied): FAIL -- join=$( [ "$join" = 0 ] && echo PASS || echo FAIL )" \
         "cap-denied=$( [ "$cap" = 0 ] && echo PASS || echo FAIL ) (see reasons above)."
    return 1
}
