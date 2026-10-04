# vms-dfe — a joiner whose connectivity blinks mid-admission, on the bench

**2026-09-25, a temporary `ovmx-lab/dfe-cn3lab` pod on `k3s-worker`, on an
ISOLATED bridge (`brdfe` + `tapCdfe`/`tapAdfe`/`tapBdfe`) created for this run
inside the pod, under QEMU/KVM. The lab's live reference cluster
(`vaxlab-0..4`) was never touched, and neither was any other lane's pod. The
pod was deleted after the run.**

This is the bench reproduction of the in-browser stall rd vms-dfe was filed
for (`tests/lab/captures/vms-e18e-cn3-browser-20260925/cn3-intermittent/`),
made deterministic, plus the A/B that attributes it.

| node | what it is | identity | tap |
|---|---|---|---|
| **VAXC** | a **real** OpenVMS VAX **V7.3** under SIMH | `VAXC`, SCSSYSTEMID 1989, VOTES 1, EXPECTED_VOTES 1 | `tapCdfe` |
| **OVMXA** | booted OVMX/x86_64 under QEMU/**KVM** | `OVMXA`, 1987, VOTES 1, EXPECTED_VOTES 2 | `tapAdfe` |
| **OVMXB** | booted OVMX/x86_64 under QEMU/**KVM** — the joiner under test | `OVMXB`, 1988, VOTES 1, EXPECTED_VOTES 3 | `tapBdfe` |

Cluster group **257**, `VAXCLUSTER=2`. `EXPECTED_VOTES > 1` on both OVMX nodes
is deliberate: neither can satisfy quorum on its own vote, so reaching MEMBER
can only be a real admission by the real VAX. `CAP_NET_RAW` is dropped from
both QEMU subtrees (`*.console.caps`), so the L2 I/O is the executive's own
kernel socket or it does not happen.

## The injected fault

The browser rig's distinguishing property is slow, jittery timing, and the
stall it produced was a joiner losing its `VMS$VAXcluster` connection about a
second after the cluster opened it. Both halves are reproduced here as things
the harness DOES, not things it hopes for:

* `tc qdisc ... netem delay 80ms 40ms distribution normal` on the joiner's tap
  from boot — the browser's timing, on the bench;
* `blackout.sh` watches the joiner's own console for the line that means the
  pair's `VMS$VAXcluster` connection is now OPEN, and at that instant drops
  **every** frame on its tap for 45 s (`netem loss 100%`), then restores it.
  netem only DROPS: nothing is injected on the wire and no frame is altered.

45 s is longer than the p. 7-30 reconnect window this node computes from its
own SYSGEN `RECNXINTERVAL` (20 s), so the window really expires while the node
is isolated, and then the LAN comes back with the cluster still there.

## The A/B — two boot-artifact sets from the same workflow, one patch apart

Both arms boot artifacts built by `.github/workflows/build-boot-artifacts.yml`
from a named commit, verified by `SHA256SUMS` in the pod before use. Nothing
else differs: same VAXC volume (restored from the pinned copy before every
arm), same bridge, same jitter, same blackout.

`states.sh` prints the ordered, deduplicated sequence of connectivity states
the joiner's OWN `SHOW CLUSTER` showed for each system. That is the whole
story in three lines.

### main (`a5cec562`) — the wedge, 5 runs of 5

```
##### CTL2-2
  1987   OPEN -> RECONNECT -> DISCONNECT
  1989   OPEN -> RECONNECT -> DISCONNECT
```

Both peers end in p. 7-24 **DISCONNECT** and stay there. `OVMXB` never becomes
a member, and `SHOW CLUSTER` on it reads:

```
| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXB  |          | VMX V0.7        | LOCAL            |
| 1987   |          |                 | DISCONNECT       |
| 1989   |          |                 | OPEN             |
```

...for the rest of the window, beside a healthy cluster whose circuit it can
still see. That is the reported stall, deterministic: **5 of 5 runs wedge.**

### the fix (`b7e57a2a`) — the block is deallocated and rebuilt

```
##### V2-3
  1987   NEW -> MEMBER
  1989   MEMBER
  OVMXB  LOCAL -> MEMBER
```

No block ever reaches DISCONNECT in **any** run of any fix arm (24 runs). A
peer the ladder gives up on comes back **NEW** — p. 7-25's "a new CSB is
created for it just as if it were joining the cluster for the first time" —
and the joiner asks again. `SHOW CLUSTER` on both OVMX nodes then names all
three systems MEMBER, and the real VAX's own CNXMAN proposes and completes the
addition itself:

```
%CNXMAN,  received VAXcluster membership request from system OVMXB
%CNXMAN,  proposing addition of system OVMXB
%CNXMAN,  completing VAXcluster state transition
```

## Every run, graded

`grade2.sh` is the grader. **`reconnect-lines` is REPORTED, not a pass
criterion**: this harness cuts the joiner's connectivity on purpose, so
"%CNXMAN, lost connection to a cluster member, reconnecting" is the correct
line for the fault that was injected. The teeth are: the joiner reached
MEMBER, BOTH OVMX nodes' own `SHOW CLUSTER` name all three systems MEMBER, the
real VAX proposed the addition itself, and nothing bugchecked.

| arm | commit | what is in it | blackout lands | runs | pass | wedged | VAX bugchecks |
|---|---|---|---|---|---|---|---|
| `CTL` | `a5cec562` (main) | — | at CM discovery | 3 | 3 | 0 | 0 |
| `CTL2` | `a5cec562` (main) | — | after the connection is OPEN | 5 | **0** | **5** | 1 |
| `FIX` | `b06e55e0` | the p. 7-25 reclaim | at CM discovery | 10 | 8 | 0 | 2 |
| `FIN` | `01737872` | + log-once | after the connection is OPEN | 4 | 0 | 0 | 1 |
| `V2` | `b7e57a2a` | + the peer-DISCONNECT edge | after the connection is OPEN | 5 | 3 | 0 | 1 |
| `NF` | `b7e57a2a` | (no blackout — jitter only) | — | 10 | **9** | 0 | 0 |

**The `NF` row is the reliability number.** Ten consecutive three-node runs on
the fixed executive with the browser's jitter but no injected fault: **nine
reached CN=3 with all three systems MEMBER on both OVMX nodes' own
`SHOW CLUSTER`, the real VAX proposing each addition itself, and ZERO
bugchecks on any console.** The tenth (`NF-7`) was still progressing when its
arm's window closed -- its last line is `adopting the VMS$VAXcluster
connection the executive holds for this member` at t=56 s -- so it is counted
as a failure rather than argued away, but it is a slow join under 80±40 ms of
jitter, not a stall: no block ever reached DISCONNECT in it either.

The `CTL` row is the reason the trigger was retargeted: landing the blackout
at CM discovery is too early — the CSB has not reached p. 7-24 OPEN, so
`h_connect_abandoned` returns it to NEW and the wedge condition is never
created. `CTL2` lands it after the connection is OPEN, and then main wedges
every time.

## Two things this run found that rd vms-dfe did not ask for

### 1. A pre-existing peer bugcheck, on BOTH arms, in the same window

`vax-bugcheck-on-main/` is a **main** run (`CTL2-4`) in which the real VAX
takes `**** Fatal BUG CHECK ... CNXMGRERR, Error detected by VAXcluster
Connection Manager` around OVMXA's admission, before the joiner has done
anything the fix could affect. The rate is the same on both sides of the A/B
(main 1/5, fix 3/19), and the crash lands in the same window in every case, so
it is **not** attributable to this item's change — it is the E81/crossing-connect
family the PR escalates, exposed here by the injected jitter. Filed as
**rd vms-b36**; nothing in this item's code runs before it.

### 2. A peer-driven connect/disconnect loop the tombstone used to mask

`peer-disconnect-loop/` (`V2-1`, with the pcap) is the residual failure mode.
After the blackout ends, the pair enters a ~2 Hz loop:

```
[ 73.56] %CNXMAN, the VMS$VAXcluster connection to a cluster member closed: remote disconnect
[ 73.57] %CNXMAN, lost connection to a cluster member, reconnecting
[ 74.06] %CNXMAN, the VMS$VAXcluster connection to a cluster member closed: remote disconnect
...
```

**Half of it is fixed here.** Routed as a plain path loss, OVMX's own reconnect
ladder was dialling into that loop: `h_conn_lost` opened a fresh 20-second
window, the beat dialled a second later, the peer hung up again and `h_open`
cleared the deadline, so the window could never expire and the node emitted a
`VMS$VAXcluster` connect roughly twice a second forever (457 cycles in one
400-second run: `FIN-1`, `FIN-3`, `FIN-4`). `CNXMAN_CSB_EV_REMOTE_DISCONNECT`
now takes E81's stop-asking edge, and the arms with it in recover instead
(`V2-2/3/4`).

**The other half is the PEER re-opening the connection and hanging up again**,
which this node can only answer (the Rule of Total Connectivity requires it).
`V2-1` still loses to it: one run in five. That is a separate defect, in the
dialogue OVMX offers on an accepted connection rather than in this item's
recovery; the pcap is kept here for it and it is filed as **rd vms-4c9**.

## What is NOT claimed

* **No in-browser run.** rd vms-dfe's pass bar is ten consecutive in-browser
  CN=3 attempts; that rig additionally needs an OVMX/VAX Node B image and a
  `pcjs` change in another repo, and was not rebuilt here. What is claimed is
  the bench A/B above and the no-fault reliability row.
* **Not a licensed VMScluster** — VAXC logs `%LICENSE-E-NOAUTH` at startup and
  admits the OVMX nodes anyway, as in every previous capture here.
* **No `SHOW CLUSTER` table from VAXC.** Its `OPA0:` stops echoing after login
  on this minimally-tailored V7.3 volume (the same harness limitation recorded
  in `vms-1ac-cn3-achieved-20260925/`). The VMS-side evidence is CNXMAN's and
  OPCOM's own console output, which is VMS reporting on itself.

## Files

* `main-wedges/` — a **main** run under the targeted blackout: the wedge, whole.
* `fix-recovers/` — the same fault on the fixed executive: all three MEMBER.
* `vax-bugcheck-on-main/` — the pre-existing CNXMGRERR, on main.
* `peer-disconnect-loop/` — the residual peer-driven loop, with its pcap.
* `dfenode.sh`, `dfestart.sh` — the in-pod node launchers (derived verbatim
  from `vms-1ac-cn3-achieved-20260925/`, only the run root differs).
* `blackout.sh` — the fault injector. `runarm.sh` — one arm. `loop.sh` — N
  arms, each graded and archived. `grade2.sh` — the grader. `states.sh` — the
  connectivity-state sequence reader that is the A/B's headline.

## Reproducing

```
# a temporary PRIVILEGED pod on k3s-worker (privileged is what gets the device
# cgroup for /dev/kvm; TCG timing confounds this work), with the vax-lab PVC:
ip link add brdfe type bridge; ip link set brdfe up
for t in tapCdfe tapAdfe tapBdfe; do ip tuntap add dev $t mode tap
  ip link set $t master brdfe; ip link set $t up; done

# artifacts, per arm: gh workflow run build-boot-artifacts.yml --ref <sha> \
#                        -f cluster_auth_group=257     (then gh run download)
# nodeC: a copy of the pinned /lab/cluster-demo-nodeC-v73 volume + a vax.ini
#        attaching xq to tapCdfe.

bash loop.sh <artifacts-dir> <TAG> 10          # ten arms, graded
NOFAULT=1 bash loop.sh <artifacts-dir> NF 10   # ten arms with no blackout
```
