# vms-b36 — the real VAX's CNXMGRERR, made deterministic, and an all-real-VMS oracle beside it

**2026-09-25, a temporary `ovmx-lab/b36lab` pod on `k3s-worker`, created for
this work and deleted after it. TWO isolated rigs inside that one pod, on two
bridges that cannot see each other:**

| rig | bridge | nodes |
|---|---|---|
| **the bench** (`/lab/run-b36`) | `brb36` + `tapCb36`/`tapAb36`/`tapBb36` | a **real** OpenVMS VAX V7.3 under SIMH (`VAXC`, SCSSYSTEMID 1989) founds; two booted OVMX/x86_64 nodes under QEMU/**KVM** (`OVMXA` 1987, `OVMXB` 1988) join. Cluster group 257, `VAXCLUSTER=2`, `EXPECTED_VOTES` 1/2/3. `CAP_NET_RAW` dropped from both QEMU subtrees. |
| **the oracle** (`/lab/k8s-labs/b36lab`) | `br0` + `tap1`/`tap2`/`tap3` | **THREE real OpenVMS VAX V7.3 nodes and nothing else** — `VAX1` (SYS0), `VAX2` (SYS1), `VAX3` (SYS2), one shared system disk cloned from the lab's own `.3node-golden` snapshot, cluster group 1. |

**The lab's live reference cluster (`vaxlab-0..4`) was never touched**, and
neither was any other lane's pod. Both rigs were built from copies.

---

## 1. What was measured, and where it differs from the filed premise

rd vms-b36 was filed off `vms-dfe-blackout-recovery-20260925/vax-bugcheck-on-main/`
and reads the crash as landing "around OVMXA's admission". **Re-read from the
kept console, it does not.** `VAXC.console.log` in that exhibit says:

```
%CNXMAN,  received VAXcluster membership request from system OVMXA
%CNXMAN,  proposing addition of system OVMXA
%CNXMAN,  completing VAXcluster state transition        <- OVMXA is IN, cleanly
%CNXMAN,  received VAXcluster membership request from system OVMXB
%CNXMAN,  proposing addition of system OVMXB            <- the transition OPENS
%CNXMAN,  lost connection to system OVMXB
%CNXMAN,  timed-out lost connection to system OVMXB
%CNXMAN,  aborting VAXcluster state transition
**** Fatal BUG CHECK, version = V7.3     CNXMGRERR, ...
```

The crash is in **OVMXB's** admission, not OVMXA's. That correction is what
made the fault reproducible: see §3.

## 2. The oracle — three REAL V7.3 nodes, the same faults

`oracle/` is the control. A real three-node VMScluster was built in this pod
(`vax3` boots system root SYS2 with `B/R5:20000000 DUA0`), and then the bench
rig's own faults were injected on `tap3` with `tc netem` — which only DROPS or
DELAYS: nothing is injected on the wire and no frame is altered.

**(a) A member that vanishes.** `vax3` killed, twice:

```
VAX1: lost connection to system VAX3 / timed-out / proposing reconfiguration
      / removed from VAXcluster system VAX3 / completing state transition
VAX2: lost connection to system VAX3 / removed from VAXcluster system VAX3
```

and on the second occasion the roles were **reversed** — VAX2 proposed and VAX1
followed. So the proposer is the first detector and it races; the loser says
nothing at all. **Zero bugchecks on either survivor, both times.**

**(b) A node blacked out for 45 s mid-admission** (`oracle/fault3.sh`, armed on
the coordinator's own "proposing addition of system VAX3" line). The survivors
removed it and completed the transition; the isolated node lost quorum,
proposed its own reconfiguration, and took

```
**** Fatal BUG CHECK, version = V7.3     CLUEXIT, Node voluntarily exiting VAXcluster
```

— which is how a real node leaves that state: it **re-incarnates**. Again **zero
CNXMGRERR on the survivors.**

**(c) THE FINDING (`analysis/oracle-accept-vs-reject.txt`).** Across the
blackout, every `VMS$VAXcluster` connect between the survivors and the removed
node is answered — and the answer changes:

```
t=   0.000  VAX2  dials VAX3   ->  ACCEPT        (VAX3 is being admitted)
t=   1.143  VAX1  dials VAX3   ->  ACCEPT
t=  52.283  VAX2  dials VAX3   ->  REJECT        (VAX3 has been removed)
t=  53.277  VAX1  dials VAX3   ->  REJECT
t=  53.897  VAX3  dials VAX2   ->  REJECT        (...and the member refuses it too)
t=  54.278  VAX1  dials VAX3   ->  REJECT
t=  55.278  VAX1  dials VAX3   ->  REJECT
t=  56.278  VAX1  dials VAX3   ->  (no answer)   (VAX3 has CLUEXITed)
```

**A real OpenVMS connection manager REJECTS an inbound `VMS$VAXcluster` connect
for a relationship it has given up on, once a second, both ways, until the peer
comes back as a new incarnation.** This is the first capture in this repo of a
real VMS node refusing that connect at all; the standing escalation in
`docs/cluster-integration-notes.md` (carried to FC-P3.3) and correction D12 in
`docs/design-cluster-book-grounding.md` both record that nothing grounded it.

**OVMX, in the same position, ACCEPTS.** See §4.

**(d) The connect data is derived, not a constant**
(`analysis/connect-data.txt`). The 16-byte SCA connect data at content
`[94:110]` on `VMS$VAXcluster`:

```
real VAX2/VAX1, MEMBERS      011b0103 01000100 02000108 00000600
real VAX3, being ADMITTED    011b0103 00000000 00000008 00000600
real VAXC alone (bench)      011b0103 01000100 01000108 00000600
real VAXC, 2 members         011b0103 02000200 02000108 00000600
real VAXC, later in the run  011b0203 02000200 02000109 02000600   <- [2] and [12] MOVE
OVMX, EVERY frame it sends   011b0103 00000000 00000008 00000600
```

Two things follow. First, the real field tracks the sender's own cluster state
and **`content[96]` and `content[106]` are not the constants §4(N) of the
protocol spec records them as** — the bench VAX moved both inside one run.
Second, OVMX emits one baked template (`cnxman_e31_conndata`) on every connect
and every accept regardless of its own state; that is the thing the operator
memory `executive-backed-not-wire-plumbing` forbids, and the spec's own §4(N)
already admits it ("OVMX copies a real joiner's observed bytes and therefore
cannot generate connect data for a role it has not captured").

## 3. The bench rig, made deterministic

`rig/` is derived from `vms-dfe-blackout-recovery-20260925/` with ONE change,
in `runarm.sh`: **where the fault lands.**

* rd vms-dfe armed on the JOINER's console line that its `VMS$VAXcluster`
  connection was open. On this build the joiner frequently loses that
  connection *before its membership request ever reaches the member*, so the
  member never opens a transition and the window the CNXMGRERR lives in is
  never entered. Measured here: `CTL-1` and `CTL-2` both ended in the peer-driven
  connect/disconnect loop with **0 bugchecks** and no transition at all.
* rd vms-b36 arms on **the real VAX's own console line that it has opened the
  transition** — `%CNXMAN,  proposing addition of system OVMXB` — and blacks the
  joiner's tap out at that instant for 45 s. Same fault, landed where the
  archived exhibit landed it.

### The A/B, twelve arms

Both arms are boot artifacts from the **same** workflow
(`build-boot-artifacts.yml`, `cluster_auth_group=257`), one patch apart,
`sha256sum -c`'d in the pod before use. Nothing else differs: same pinned VAXC
volume restored before every arm, same bridge, same trigger, same 45 s.

| arm | commit | what is in it | arms | fault injected | CN=3 | **VAX bugchecks** |
|---|---|---|---|---|---|---|
| `M1` | `7d053b50` (main) | — | 6 | 6 | 4 | **2** |
| `F1` | `9d94dde5` | the never-admitted-removal gate | 6 | 5¹ | 3 | **2** |

¹ `F1-4`'s marker never appeared (the VAX never opened a transition for OVMXB
in that arm), so no fault was injected in it and it is not an arm of the
experiment. It is reported rather than dropped.

**The gate this branch lands does not change the bugcheck rate on this rig, and
this table says so.** The vector this rig exposes is §4's accept, not the
removal: in every crashing arm of either colour `proposing removal of a system
from the cluster` never appears at all — the VAX dies before the joiner's
reconnect window ever expires. What the gate closes is the vector the ARCHIVED
`vms-dfe-blackout-recovery-20260925/vax-bugcheck-on-main/` run shows, where the
window did expire and the removal was proposed 0.6 s before the VAX went.

The window is entered **every** run now — against 1 run in 5 on the rd vms-dfe
trigger. That is the rig this item needed, and it is what makes §4 readable at
all.

## 4. The crash frame

`analysis/crash-window-M1-2.txt`, produced by
`tools/cluster/cm_crash_window.py`, is the whole answer. The pair is in the
rd vms-4c9 connect/disconnect loop, and then:

```
[2277] -0.001 s  VAXC  -> OVMXB  CONNECT_REQ  rem=00000000 loc=dbf40008  VMS$VAXcluster
[2279] -0.001 s  OVMXB -> VAXC   CONNECT_RSP  rem=dbf40008 loc=00000000
[2280] -0.001 s  OVMXB -> VAXC   ACCEPT_REQ   rem=dbf40008 loc=325a0052  VMS$VAXcluster
[2282] -0.000 s  VAXC  -> OVMXB  ACCEPT_RSP   rem=325a0052 loc=dbf40008
[2283]  0.000 s  VAXC  -> AB-00-04-01-01-02   msgtype 0xb1   <- the last gasp
```

**OVMX ACCEPTS the connect. 0.3 ms after the connection completes, the real VAX
puts its last-gasp datagram on the cluster multicast and bugchecks CNXMGRERR.**
In the same position a real V7.3 node sends `REJECT_REQ` (§2(c)).

`analysis/crash-window-F1-2.txt` and `crash-window-F1-3.txt` are the SAME four
frames on the **fixed** arm, which is what makes the shape a signature rather
than one run's accident:

```
CONNECT_REQ (VAXC) -> CONNECT_RSP + ACCEPT_REQ (OVMXB) -> ACCEPT_RSP (VAXC) -> 0xb1
```

`M1-1` is the same family one step earlier: OVMXB accepts a **second**
`VMS$VAXcluster` connection (`loc=e09a0053`) from the VAX while the first
(`loc=e09a0008`) has never been disconnected on the wire — the VAX had been
sending CM messages on that first pair, unanswered, once every three seconds
for 35 s — and the VAX dies moments later.

**So rd vms-b36 and rd vms-4c9 are two outcomes of ONE defect:** OVMX answers
`ACCEPT_REQ` where a real connection manager answers `REJECT_REQ`. Sometimes the
peer merely hangs up (rd vms-4c9's 2 Hz loop, 460 cycles in one arm here);
sometimes completing the connection tips its CM into CNXMGRERR.

`analysis/` also records the 16-bit code on those loop disconnects: the VAX's
460 loop `DISCONNECT_REQ`s carry `0x8004`, where its ordinary disconnects carry
`0x0000`/`0x0001` and its `REJECT_REQ` carries `0x002c`.

## 5. What was FIXED in this item, and what was not

**Fixed and proven (R1 + R2):** this executive proposed a class-0x03 removal
transition for a system the cluster had **never admitted** — p. 7-49's SELECTED
flag clear, no membership to remove. In the archived bugcheck run the line
`proposing removal of a system from the cluster` appears in exactly the one arm
of four that bugchecked a peer, 0.6 s after the VAX abandoned that very
system's admission; and the oracle above shows real VMS abandoning such a
joiner and never removing it. `tests/cluster/host/test_cnxman_csb.c`,
`test_cnxman_coord.c` and
`tests/cluster/sim/scenarios/cnxman_never_admitted_not_removed.c` all go red
with the gate stubbed out.

**NOT fixed, and escalated rather than guessed:** the ACCEPT-vs-REJECT predicate
of §2(c)/§4. What is now grounded is *that* a real connection manager refuses,
and in which two situations it was observed refusing (a member refusing a system
it has removed; a removed node refusing everything until it re-incarnates). What
is **not** grounded is the predicate an executive should evaluate on its own
state to decide, and inventing one is exactly what Rule 8 and INV-6 forbid —
especially on the accept path, where refusing a peer's legitimate p. 7-24
REACCEPT would break every recovery this lane has built. It is also visibly
entangled with a second missing behaviour: a real node in OVMXB's position
**CLUEXITs and reboots**, and OVMX has no CLUEXIT.

## Reproducing

```
# a temporary PRIVILEGED pod on k3s-worker (privileged is what gets the device
# cgroup for /dev/kvm; TCG timing confounds this work), with the vax-lab PVC.

# the bench
ip link add brb36 type bridge; ip link set brb36 up
for t in tapCb36 tapAb36 tapBb36; do ip tuntap add dev $t mode tap
  ip link set $t master brb36; ip link set $t up; done
# artifacts: gh workflow run build-boot-artifacts.yml --ref <branch> \
#                -f cluster_auth_group=257   (then gh run download, sha256sum -c)
MARK_LOG=/lab/run-b36/VAXC.console.log \
MARK="%CNXMAN,  proposing addition of system OVMXB" \
  bash rig/startloop.sh <artifacts-dir> <TAG> 6

# the oracle (three REAL V7.3 nodes)
POD_NAME=<pod> LAB_ROOT=/lab NODES="vax1 vax2" \
  GOLDEN_SUFFIX=".3node-golden.bak" bash /usr/local/bin/entrypoint.sh &
# then stage vax3 on tap3 and boot system root SYS2:
#   nodedrv.py <dir> <log> --boot "B/R5:20000000 DUA0"
TAG=f1 bash oracle/fault3.sh
```
