# vms-1ac — ⭐ CN=3: two OVMX nodes and a real OpenVMS VAX V7.3, all three MEMBER

**2026-09-25, a temporary `ovmx-lab/vms1ac-lab` pod on `k3s-worker`, on an
ISOLATED bridge (`br1ac` + `tapC1ac`/`tapA1ac`/`tapB1ac`) created for this run
inside the pod, under QEMU/KVM. The lab's live group-1 reference cluster
(`vaxlab-1/2/3`) was never touched, and neither was any other lane's pod.**

| node | what it is | identity | tap |
|---|---|---|---|
| **VAXC** | a **real** OpenVMS VAX **V7.3** under SIMH | `VAXC`, SCSSYSTEMID 1989, VOTES 1, EXPECTED_VOTES 1 | `tapC1ac` |
| **OVMXA** | booted OVMX/x86_64 under QEMU/**KVM** | `OVMXA`, 1987, VOTES 1, EXPECTED_VOTES 2 | `tapA1ac` |
| **OVMXB** | booted OVMX/x86_64 under QEMU/**KVM** | `OVMXB`, 1988, VOTES 1, EXPECTED_VOTES 3 | `tapB1ac` |

Cluster group **257**, `VAXCLUSTER=2`. `EXPECTED_VOTES > 1` on both OVMX nodes
is deliberate: neither can satisfy quorum on its own vote, so neither can found
a cluster, and reaching MEMBER can only be a real admission. **`CAP_NET_RAW` is
dropped from both QEMU subtrees** (`*.caps`: `CapEff/CapBnd =
000001ffffffdfff`, bit 13 clear), so a userspace `AF_PACKET` open would
`EPERM` — the L2 I/O is the executive's kernel socket or it does not happen.

## The result

**All three systems MEMBER, sustained 13 minutes, zero bugchecks, and the real
VAX proposing and completing both additions itself.**

`OVMXA.console.log` — OVMX's own `SHOW CLUSTER`, last of 53 polls:

```
View of Cluster from system ID 1987 node: OVMXA    24-SEP-2026 23:56:07
| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXA  | 00010002 | VMX V0.7        | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |
| 1988   | 00010003 |                 | MEMBER           |
```

`OVMXB.console.log` — the same cluster from the third node:

```
View of Cluster from system ID 1988 node: OVMXB    24-SEP-2026 23:56:09
| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXB  | 00010003 | VMX V0.7        | MEMBER           |
| 1987   | 00010002 |                 | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |
```

and its own connection manager counting all three:

```
[   23.251542] %CNXMAN, this node is now a VAXcluster member
[   23.254793] %CNXMAN, system 00000000000007c4 was added to the cluster
[   23.258608] %CNXMAN, system 00000000000007c3 was added to the cluster
[   23.262611] %CNXMAN, system 00000000000007c5 was added to the cluster
[   23.413556] %CNXMAN, completed VAXcluster state transition
```

`0x7c3` = 1987, `0x7c4` = 1988, `0x7c5` = 1989.

**The oracle — the real VAX's own CNXMAN and OPCOM** (`VAXC.console.log`),
which is VMS reporting on itself, for BOTH admissions:

```
%CNXMAN,  received VAXcluster membership request from system OVMXA
%CNXMAN,  proposing addition of system OVMXA
23:54:44.54 Node VAXC (csid 00010001) proposed addition of node OVMXA
%CNXMAN,  completing VAXcluster state transition

%CNXMAN,  received VAXcluster membership request from system OVMXB
%CNXMAN,  proposing addition of system OVMXB
23:55:19.97 Node VAXC (csid 00010001) proposed addition of node OVMXB
%CNXMAN,  completing VAXcluster state transition
```

**Stability.** `grep -c 'Fatal BUG CHECK' VAXC.console.log` → **0**. Neither
OVMX console logged `lost connection`, `quorum lost` or `was removed from the
cluster` — **0 each** — across a window whose own poll markers reach
`CN3-POLL-OVMXA-795` and `CN3-POLL-OVMXB-765`, i.e. 13m15s and 12m45s of
sustained membership after the third node was admitted. The VAX was still
putting SCA frames on the bridge at the end of it.

## Both rd vms-4f0 and rd vms-1ac, visible in one dialogue

`cn3.pcap`, decoded with `cn3dec.py`, the third node's admission:

```
 30.3137 OVMXB>OVMXA  cat=01 op=02            the third node asks the OVMX member
                                              ... and OVMXA SILENTLY DISCARDS it:
                                              no answer, no relay, no transition
 36.4508 OVMXB>VAXC   cat=01 op=02            6.1 s later the joiner asks the real VAX
 36.4516 VAXC >OVMXA  cat=01 op=12 epoch=3    the VAX relays, at the CURRENT epoch
 36.4519 OVMXA>VAXC   cat=81 op=12 epoch=3    OVMXA ANSWERS  <-- rd vms-4f0
 36.4521 VAXC >OVMXA  cat=01 op=03 epoch=4    commit, epoch advanced
 36.4525 VAXC >OVMXB  cat=01 op=03 epoch=4
 36.4530 VAXC >OVMXB  cat=01 op=05 epoch=4    membership records, at the transition epoch
 36.4692 VAXC >OVMXA  cat=01 op=09 epoch=4    the ADD open
 36.4928 VAXC >{A,B}  cat=01 op=0a epoch=4    GO
```

Two things this run settles that no previous one could:

1. **rd vms-4f0's member-side answer is exercised on the wire.** The earlier
   run (`vms-4f0-cn3-relay-20260924/`) never reached it, because the third node
   always asked the OVMX member and the OVMX member always took the job. Here
   the op-0x12 relay comes from the real VAX and OVMXA answers it in 0.3 ms —
   the frame the whole admission gates on.
2. **rd vms-1ac's selection gate is what makes that happen.** OVMXA is
   outranked by VAXC (1989 > 1987), so it discards the op-0x02 in silence, and
   the joiner's own admission-silence clock re-issues to VAXC. Compare the
   previous run, where OVMXA took it, appointed itself coordinator, and the
   real VAX bugchecked `CNXMGRERR` 1.3 ms after its op-0x09.

**And the real VAX independently confirms this branch's epoch phasing**: its
own relay carries epoch 3 and everything from the commit on carries 4 — exactly
what `coord_advance_epoch()` now does, derived from
`vax3-2to3-established-join-20260730.pcap` before this run existed.

## The selection census

`coord-selection-census.txt` is the evidence behind the receiver-side gate,
over both reference capture trees: of **113** ADD admissions with an observed
op-0x12 relay, the coordinator was the **highest-SCSSYSTEMID live member** in
**107**. All six exceptions are captures in which the higher-numbered "member"
is an OVMX strawman node, and in two of those (`d94-by7`, `d94-by8`) it had
never been admitted at all — it sent no cat-0x01 membership traffic in one and
only its own op-0x01/op-0x02 in the other.

The predicate is labelled **INFERRED**, as the joiner-side pick already is
(spec §4(p): "highest DECnet node number ... confounded with highest
SCSSYSTEMID"). What makes it safe to act on is the direction of the error: a
node that wrongly defers coordinates nothing and crashes nobody, and the
joiner re-issues — which is exactly what is measured above, 6.1 s apart.

## What is NOT claimed

* **No `SHOW CLUSTER` table from VAXC.** Its OPA0: stops echoing after login on
  this minimally-tailored V7.3 volume (no TERMTABLE entry for the console — the
  same harness limitation recorded in `vms-b34-group-on-wire-20260924/`). The
  VMS-side evidence here is CNXMAN's and OPCOM's own console output instead,
  which is VMS reporting on itself for both admissions, and the two OVMX
  `SHOW CLUSTER` tables that name it MEMBER.
* **Not a licensed VMScluster** — VAXC logs `%LICENSE-E-NOAUTH` at startup and
  admits the OVMX nodes anyway, as in every previous capture here.
* This run does **not** exercise OVMX as the coordinator: the whole point of
  the selection gate is that it defers to the real VAX. OVMX-coordinated
  admissions are covered at rung R1 (`test_cnxman_coord.c`), and the
  `op 0x09` fields OVMX cannot yet build are refused rather than guessed
  (`CNXMAN_COORD_REF_OPEN_UNGROUNDED`).

## Files

* `OVMXA.console.log`, `OVMXB.console.log`, `VAXC.console.log` — all three
  consoles, whole run.
* `OVMXA.console.caps`, `OVMXB.console.caps` — the live `/proc` capability sets
  of the booted-node QEMU subtrees.
* `cn3.pcap` — every 0x6007 frame on the isolated bridge.
* `cn3node.sh`, `cn3start.sh` — the in-pod launchers (one node each, detached,
  QEMU/KVM, `capsh --drop=cap_net_raw`).
* `cn3dec.py` — the CM-dialogue decoder used above.
* `coord_add.py`, `coord-selection-census.txt` — the selection census and the
  script that produces it from the reference trees.

## Reproducing

```
# a temporary PRIVILEGED pod on k3s-worker (privileged is what gets the device
# cgroup for /dev/kvm; TCG timing confounds this work), with the vax-lab PVC:
ip link add br1ac type bridge; ip link set br1ac up
for t in tapC1ac tapA1ac tapB1ac; do ip tuntap add dev $t mode tap
  ip link set $t master br1ac; ip link set $t up; done

# artifacts: docker build -f distro/Dockerfile.bootable -o dist .
# per node:  tools/cluster-web-demo/inject-cluster-config.sh <in> <out> \
#              --scsnode OVMXA --scssystemid 1987 --votes 1 \
#              --expected-votes 2 --group 257

bash cn3start.sh cap
bash cn3start.sh C                     # SIMH real V7.3, boot B DUA0
ART_ROOT=<artdir> bash cn3start.sh A   # wait for MEMBER
ART_ROOT=<artdir> bash cn3start.sh B   # the third node
```
