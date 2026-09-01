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
# lj_node_status <showcluster_txt> <node-or-id> -> prints that node's STATUS column
# (upper-case), empty if the node is not in the table. Parses the VMS SHOW CLUSTER
# table row "| NODE | SOFTWARE | STATUS |" by COLUMN -- never a loose "MEMBER
# appears somewhere on/near the line" match. The old grep-MEMBER-on-the-line check
# false-passed by matching the "| MEMBERS |" header or a wrapped/garbled console
# frame, and by treating a transient hit as admission (lab-2 vms-a84d: it reported
# OVMXJ0=member while every sustained read was NEW). Column extraction makes NEW /
# BRK_NON / OPENING distinguishable from MEMBER, which is the whole point.
lj_node_status() {
    # Normalise pipe-table ("| OVMXJ0 | VMX V0.1 | NEW |") and bare space-separated
    # ("OVMXJ0 MEMBER") rows alike: pipes -> spaces, then on the node's OWN line find
    # the FIRST status keyword AFTER the node token. Restricting to the node's own
    # line + keyword-after-node is what stops the cross-line / wrapped-console
    # false-match ("MEMBERS" header, a different node's MEMBER) that latched a false
    # admission. "MEMBERS" (plural) never matches the exact "MEMBER".
    # Return the LAST matching row's status (the console log is cumulative -- many
    # SHOW CLUSTER dumps concatenated -- so the most recent one is authoritative).
    printf '%s\n' "$1" | tr -d '\r' | tr '|' ' ' | awk -v n="$2" '
        {
            nn=toupper(n); found=0
            for (i=1;i<=NF;i++) {
                tok=toupper($i)
                if (found==1 && (tok=="MEMBER"||tok=="NEW"||tok=="REMOVED"||tok=="OPENING"||tok=="OPEN"||tok ~ /^BRK/)) {
                    last=tok; break
                }
                if (tok==nn) found=1
            }
        }
        END { if (last!="") print last }'
}

# lj_csb_status <sda_showcluster_txt> <node> -> that node's SDA CSB Status flags
# (lower-case), empty if absent. SDA (ANALYZE/SYSTEM -> SHOW CLUSTER) is the ORACLE
# that survives when the interactive DCL SHOW CLUSTER WEDGES mid-transition (the DCL
# table degrades to empty/blocks while a CSB is transitioning). The CSB summary row
# is "<8hexAddr>  <Node>  <CSID>  <Votes>  <State>  <Status-flags>", e.g.
#   879BCAC0  OVMXJ0  00010004  0  wait   long_break,removed   (broken -> NOT member)
#   8794EC40  VAX2    00010002  0  open   member               (admitted)
# A node is an ADMITTED MEMBER iff its CSB Status carries the "member" flag and NOT a
# break/removed flag (long_break/break/removed = the un-acked-reject broken state).
lj_csb_status() {
    # Scope to the LIVE (highest) csid: each join gets a fresh incrementing csid, and
    # departed runs leave STALE residual CSBs (e.g. 00010003/00010004 at long_break,
    # removed) that pile up. Grading a stale residual would mis-verdict, so pick the
    # node's MAX-csid row. Status = the flag field(s) after State; when the summary
    # Status column is empty (State=open, a selected-not-yet-member transitional), fall
    # back to the State token so the read is still informative (and is_member correctly
    # sees no "member" flag).
    printf '%s\n' "$1" | tr -d '\r' | awk -v n="$2" '
        $1 ~ /^[0-9A-Fa-f]{8}$/ && toupper($2)==toupper(n) {
            csid=$3; st=""
            for (i=6;i<=NF;i++) st=st (st==""?"":" ") $i
            if (st=="") st=$5
            if (csid > maxcsid) { maxcsid=csid; maxst=tolower(st) }
        }
        END { if (maxcsid!="") print maxst }'
}

# lj_csb_is_member <sda_showcluster_txt> <node> -> exit 0 if the node is an admitted
# cluster MEMBER per its SDA CSB (member flag present, no break/removed).
lj_csb_is_member() {
    local st; st="$(lj_csb_status "$1" "$2")"
    case ",$st," in
        *,member,*|*,member) : ;;   # carries the member flag
        *) return 1 ;;
    esac
    case "$st" in
        *long_break*|*break*|*removed*) return 1 ;;   # broken/removed -> not clean member
    esac
    return 0
}

lj_verdict() {  # <ovmx_node> <ovmx_showcluster_txt> <vax_showcluster_txt> <cn> <pcap>
    local node="$1" ovmx_sc="$2" vax_sc="$3" cn="$4" pcap="$5"
    local ok=1
    local a=0 b=0 c=0 d=0
    node="$(printf '%s' "$node" | tr '[:lower:]' '[:upper:]' | cut -c1-6)"

    # (a) OVMX side sees a VAX PEER as a member (executive membership present).
    # OVMX may render peers by node name (VAX1) or by numeric SCSSYSTEMID
    # (1025/1026) -- accept either shown as MEMBER. This is OVMX's OWN reciprocal
    # view; it is NOT the admission authority. OVMX can self-report MEMBER ahead of
    # the real VAX (an over-claim) -- that is caught by leg (b), which reads the
    # VAX's authoritative view. So (a) AND (b) = OVMX sees the VAXes AND the VAX
    # admits OVMX; (a) alone can never carry a cut.
    local peer peerstat a_peer=""
    if grep -qaiE 'NOSUCHDEV' <<<"$ovmx_sc"; then
        echo "  verdict: (a) FAIL -- OVMX SHOW CLUSTER reported NOSUCHDEV: the executive/SCS is"
        echo "                       not up (expected pre-vms-5ad: booted node does not auto-start SCS yet)"
    else
        # A VAX peer must appear with STATUS==MEMBER in OVMX's own table (by node
        # name VAX1/VAX2 or by SCSSYSTEMID 1025/1026). Column-exact -- not a loose
        # MEMBER match. (a) is only OVMX's reciprocal view; (b) is the authority.
        for peer in VAX1 VAX2 ${LJ_RESERVED_IDS:-1025 1026 1027}; do
            peerstat="$(lj_node_status "$ovmx_sc" "$peer")"
            [ "$peerstat" = "MEMBER" ] && { a_peer="$peer"; break; }
        done
        if [ -n "$a_peer" ]; then
            a=1; echo "  verdict: (a) PASS -- OVMX SHOW CLUSTER lists VAX peer $a_peer as MEMBER (executive membership)"
        else
            echo "  verdict: (a) FAIL -- OVMX SHOW CLUSTER shows no VAX peer with STATUS==MEMBER (not joined from OVMX's side)"
        fi
    fi

    # (b) VAX side (the oracle) lists the OVMX node AS A MEMBER. Presence alone is
    # NOT admission: a real VAX creates a CSB for -- and counts in CLUSTER_NODES --
    # any node it has merely HEARD, showing it BRK_NON/NEW until it actually
    # promotes it to MEMBER. Only STATUS==MEMBER on the VAX's OWN SHOW CLUSTER is
    # the authentic admission (lab-2 vms-a84d: OVMXJ0 showed BRK_NON = known, not
    # admitted, while OVMX self-rendered MEMBER ahead of the VAX). This is the
    # authoritative gate -- it also catches an OVMX-side over-claim, since it reads
    # the VAX's view, not OVMX's self-report.
    # PREFER the SDA CSB (ANALYZE/SYSTEM -> SHOW CLUSTER): it is the oracle that
    # survives DCL SHOW CLUSTER wedging mid-transition, and its "member" flag is the
    # true admitted-member signal ("long_break"/"removed"/"new" = not admitted, e.g.
    # the un-acked-reject broken state lab-2 vms-2f3 held OVMXJ0 in). Fall back to the
    # DCL STATUS column only when no SDA CSB row for the node is present.
    local vstat csbstat
    csbstat="$(lj_csb_status "$vax_sc" "$node")"
    if [ -n "$csbstat" ]; then
        if lj_csb_is_member "$vax_sc" "$node"; then
            b=1; echo "  verdict: (b) PASS -- vax1 SDA CSB shows $node admitted MEMBER (flags: $csbstat)"
        else
            echo "  verdict: (b) FAIL -- vax1 SDA CSB shows $node NOT admitted (flags: $csbstat -- long_break/removed/new;"
            echo "                       the VAX made a CSB but never promoted it to a clean MEMBER)"
        fi
    else
        vstat="$(lj_node_status "$vax_sc" "$node")"
        if [ "$vstat" = "MEMBER" ]; then
            b=1; echo "  verdict: (b) PASS -- vax1 SHOW CLUSTER shows $node STATUS==MEMBER (the real VAX ADMITTED it)"
        elif [ -n "$vstat" ]; then
            echo "  verdict: (b) FAIL -- vax1 SHOW CLUSTER shows $node STATUS=$vstat, not MEMBER (known/NEW/BRK_NON)"
        else
            echo "  verdict: (b) FAIL -- vax1 shows no admitted $node (SDA CSB absent + DCL table has no $node row)"
        fi
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
