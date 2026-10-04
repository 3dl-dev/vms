# CN=2: a booted OVMX node joins a real SINGLE-NODE OpenVMS genesis VMScluster

**rd vms-b34. lab-2 `vaxlab-4`, 2026-09-20. Two runs, one variable.**

> ### ⚠ CORRECTION (rd vms-147, 2026-09-22) — the joins below are real; the group NUMBER they are labelled with is not
>
> Every measurement in this note stands: a real OpenVMS V7.3 node really did
> admit a real booted OVMX node to MEMBER on its own SDA CSB, with CAP_NET_RAW
> denied. What is **wrong** is the name this note gives the group.
>
> This note calls the cluster "group 257" because it read the number back OUT of
> VAX1's multicast address `AB-00-04-01-01-01` using OVMX's own derivation,
> which was **defective**: it built `AB-00-04-01-<LE16(group)>`, while real VMS
> builds `AB-00-04-01-<LE16(group + 0x100)>`. **The lab cluster is group 1** —
> VMS prints that itself (`SYSMAN> CONFIGURATION SHOW CLUSTER_AUTHORIZATION` on
> VAX1: `Cluster group number: 1`, `Multicast address: AB-00-04-01-01-01`).
> Staging `257` into OVMX and having it land on group 1's address was **two
> errors cancelling**, not a correct configuration — which is why the join
> worked here and would have worked on a strictly-filtering NIC too: both nodes
> ended up on the same address by accident.
>
> **So: this run did NOT depend on a permissive datalink.** The defect was
> invisible here because the compensating mislabel put OVMX on the right
> address anyway. It became visible the moment a peer's group number was a
> *real* VMS configuration rather than a back-derived one (the browser demo's
> Node C, a real V5.5-2H4 volume configured for group 257, which therefore
> transmits to `AB-00-04-01-01-02`).
>
> **Reproducing this run on corrected code: stage group `1`, not `257`.**
> Derivation, oracles and the fix: `docs/cluster-integration-notes.md` **E87**,
> `docs/cluster-protocol-spec.md` §3, `tests/cluster/host/test_codec_hello.c`.
> Every "group 257" below should be read as "group 1"; the addresses, frame
> counts and verdicts are unchanged.

## The reference cluster

`vaxlab-4`'s `vax1` alone, booted conversationally (`B/R5:1 DUA0`) with
`EXPECTED_VOTES=1` / `VOTES=1` / `VAXCLUSTER=2` — a **real OpenVMS VAX V7.3
node that founded a one-member VMScluster on its own vote**, `SHOW CLUSTER`
reading a single `VAX1 | VMS V7.3 | MEMBER` and `F$GETSYI("CLUSTER_NODES")`
reading 1. `vax2` was not running. This is the topology vms-b34 names: an
established GENESIS coordinator, not a multi-node cluster.

## The node under test

The **shipped V0.7 release artifacts**, unmodified (`vmlinuz`,
`initramfs-ovmx-slim.cpio.gz`, `ovmx-distrib.img` from the V0.7 GitHub
release), booted under QEMU/TCG inside the pod on `tap4`/`br0` with
`SCSNODE=OVMXB4 SCSSYSTEMID=1834 VAXCLUSTER=2 VOTES=1 EXPECTED_VOTES=2`.
`EXPECTED_VOTES=2` is deliberate: this node's own vote cannot satisfy quorum,
so it **cannot found a cluster of its own** — reaching MEMBER can only be a
real admission by VAX1.

**CAP_NET_RAW was DROPPED from the whole QEMU subtree in both runs**
(`CapEff/CapBnd = 00000000a80415fb`, see `*.caps`), so a userspace AF_PACKET
raw open would EPERM: the L2 I/O is the executive's kernel socket or it does
not happen.

    tests/lab/tools/labjoin_booted.sh vaxlab-4 <tag> <artifacts> 420 OVMXB4 1834
      LJ_CN_BASE=1 LJ_CN_JOINED=2 OVMX_VOTES=1 OVMX_EXPECTED_VOTES=2

## Run 1 — stock V0.7: NO JOIN, and the reason is on the wire

`nogroup-node.log`, `nogroup.caps`, census in `frame-census.txt`.

    225  52:54:00:00:00:f4  ->  ab:00:04:01:00:00      OVMX,  group 0
    180  aa:00:04:00:01:04  ->  ab:00:04:01:01:01      VAX1,  group 257

Not one frame in either direction between them. The LAVC HELLO multicast
address **is** the cluster group number — `AB-00-04-01-<lo>-<hi>` — and the two
nodes were shouting into different groups for the whole 195 s window:

* OVMX `SHOW CLUSTER/LOCAL_PORTS`: `channels 0, circuits 0`,
  `frames tx 222 (errors 0), rx 0`.
* OVMX `CNXTRACE` (the executive's own join ring, read through
  `VMS_IOCTL_CLUSTER_DIAG_JOIN`): `join state=IDLE failure=none`,
  `held=0 recorded=0 dropped=0` — **zero records**. The join FSM was never
  offered a target because the port never received a frame. Nothing in
  CNXMAN, the join FSM or the CSB ladder was the defect.
* VAX1: `CLUSTER_NODES=1` at every sample; no CSB for OVMXB4.

Why group 0: the shipped initramfs carries an **empty `/etc/ovmx`** — no
`CLUSTER_AUTHORIZE.DAT` — and the executive built the multicast address from
the unset `auth_group` anyway, with `auth_valid` (the flag that says a record
was really read) having **no reader anywhere in the executive**. Nothing on
any surface distinguished "group 0 because configured" from "group 0 because
nothing was".

The change that ships with this capture makes both facts visible — one line at
port start and one in `SHOW CLUSTER/LOCAL_PORTS`, naming the group and saying
when the 0 is a default rather than a choice. It deliberately does NOT yet
refuse to use an unconfigured group: OVMX has no operator path that authors a
CLUSTER_AUTHORIZE record (`CLUSTER_CONFIG_LAN.COM` authors SCSNODE/SCSSYSTEMID/
VOTES and not the group), so refusing would leave the documented two-node
procedure unable to form any cluster — a must-not-skip CI gate. See integration
note **E86** for the escalation and `tests/cluster/host/test_pe_glue_group.c`
for the R1 cover.

## Run 2 — the SAME artifacts + the cluster's real group: MEMBER

`group257-node.log`, `group257.caps`, `vax1-sda-and-showcluster.txt`. The only
difference is a `CLUSTER_AUTHORIZE.DAT` for group 257 staged into the
initramfs by `tests/lab/tools/stage_cluster_group.sh` (which authors it with
the project's own `tools/cluster/mk_cluster_authorize.c`). VAX1's group was
read off the wire from VAX1's own multicast address, never guessed (Rule 8).

    1062  52:54:00:00:00:f4 -> aa:00:04:00:01:04      OVMX -> VAX1, directed
     914  aa:00:04:00:01:04 -> 52:54:00:00:00:f4      VAX1 -> OVMX, directed
     253  aa:00:04:00:01:04 -> ab:00:04:01:01:01      VAX1, group 257
     245  52:54:00:00:00:f4 -> ab:00:04:01:01:01      OVMX, group 257

**MEMBER at t+15 s, CLUSTER_NODES=2 from t+30 s, sustained to window end
(t+195 s).**

VAX1's own view — the oracle, not an OVMX self-report:

    +--------+----------+---------+
    | VAX1   | VMS V7.3 | MEMBER  |
    | OVMXB4 | VMX V0.7 | MEMBER  |
    +--------+----------+---------+

    --- OVMXB4 Cluster System Block (CSB) 879DC6C0 ---
    State:  01 open
    Flags:  02020002 member,selected,status_rcvd
    SWVers: VMX V0.7
    CSID            00010002

OVMX's own view, read from the executive membership block:

    View of Cluster from system ID 1834 node: OVMXB4
    | OVMXB4 | 00010002 | VMX V0.7        | MEMBER           |
    | 1025   | 00010001 |                 | MEMBER           |

    PEA0:  open, link up
           MTU 1500, channels 1, circuits 1
           frames tx 1298 (errors 0), rx 1127 (dropped: nobuf 0, badclass 0)

Harness verdict: legs (a) OVMX sees the VAX as MEMBER, (b) VAX1's **SDA CSB**
shows OVMXB4 admitted `member`, (c) `CLUSTER_NODES=2`, (d) the join is on the
wire under the OVMX identity, and (e) CAP_NET_RAW denied — **all five PASS**.

## Run 3 — the SAME join on the code this change ships

`fixed-group257-node.log`, `fixed-group257.caps`,
`vax1-fixed-sda-and-showcluster.txt`, third census block in
`frame-census.txt`. Artifacts built from the branch (not the release), group
257 staged the same way, `SCSNODE=OVMXB6 SCSSYSTEMID=1836`, window 135 s.

**All five legs PASS again** — `OVMXB6` admitted MEMBER on VAX1's own SDA CSB
(`Flags: 02020002 member,selected,status_rcvd`), `CLUSTER_NODES=2` from t+30 s,
sustained to window end, CAP_NET_RAW denied. No regression: the change is
report-only.

And the diagnosis that was missing is now on the boot console, three lines
before the one that used to be the whole story:

    %CNXMAN, cluster group 257 (CLUSTER_AUTHORIZE)
    %CNXMAN, waiting to form or join an OpenVMS Cluster

...and on the surface an operator asks:

    PEA0:  open, link up
           MTU 1500, channels 1, circuits 1
           cluster group 257 (CLUSTER_AUTHORIZE)
           frames tx 969 (errors 0), rx 824 (dropped: nobuf 0, badclass 0)

**Honestly scoped:** the UNCONFIGURED wording of those two lines
(`cluster group 0 is NOT CONFIGURED ...`) was not re-observed on a booted node
— it is the other branch of the same two `if`s, on the same already-proven
`CLUSTER_DIAG_PORT` values, and it is pinned by the source scans in
`tests/cluster/host/test_cnxman_boot.c` and `test_pe_glue_group.c`. Run 1
above is what that node looks like today, minus the two new lines.

## Never-crash-a-peer

VAX1 survived both runs and the teardown with **zero bugchecks**. The run-2
teardown kills QEMU (an unclean departure by construction), and VAX1 did the
correct VMS thing: `lost connection to system OVMXB4` → `proposing
reconfiguration` → `removed from VAXcluster system OVMXB4` → `completing
VAXcluster state transition`. It then sat quorum-blocked, which is what a
one-vote node with `EXPECTED_VOTES=2` must do after losing the other vote —
normal, documented VMS behaviour, not a fault.

## What this does NOT establish

This is the lab topology, not the browser demo. The demo's Node C is OpenVMS
**5.5** in an in-page emulator with an in-page L2 hub, and its own run
(`../cn2-nodea-vaxc-20260920/`) shows Node A's console AND its wire going
silent together at t≈296-330 s — a node-level stall that is a separate,
unmeasured question. That run's README describes Node A as "otherwise alive
and responsive", which its own log contradicts: Node A's last console output
of any kind is at t=296 s, and the 12 `SHOW CLUSTER` requests over the
following 8.5 minutes got no reply while Node C answered all 11 of its own.
