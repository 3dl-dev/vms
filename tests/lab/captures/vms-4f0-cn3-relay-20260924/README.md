# vms-4f0 — the unanswered op-0x12 RELAY, and a SECOND defect the lab found

**2026-09-24, a temporary `ovmx-lab/vms4f0-lab` pod on `k3s-worker`, on an
ISOLATED bridge (`br4f0` + `tapC4f0`/`tapA4f0`/`tapB4f0`) created for this run
inside the pod. The lab's live group-1 reference cluster (`vaxlab-1/2/3`) was
never touched, and neither was any other lane's pod.**

Three machines, one L2 bridge, cluster group **257**, `VAXCLUSTER=2`:

| node | what it is | identity | tap |
|---|---|---|---|
| **VAXC** | a **real** OpenVMS VAX **V7.3** under SIMH | SCSNODE `VAXC`, SCSSYSTEMID 1989, VOTES 1, EXPECTED_VOTES 1 | `tapC4f0` |
| **OVMXA** | booted OVMX/x86_64 under QEMU/TCG | `OVMXA`, 1987, VOTES 1, **EXPECTED_VOTES 2** | `tapA4f0` |
| **OVMXB** | booted OVMX/x86_64 under QEMU/TCG | `OVMXB`, 1988, VOTES 1, **EXPECTED_VOTES 3** | `tapB4f0` |

`EXPECTED_VOTES > 1` on both OVMX nodes is deliberate: neither can satisfy
quorum on its own vote, so neither can found a cluster, and reaching MEMBER
can only be a real admission. `CAP_NET_RAW` is **dropped** from both QEMU
subtrees (`*.caps`: `CapEff/CapBnd = 00000000a80415fb`, `net_raw` clear), so a
userspace `AF_PACKET` open would `EPERM` — the L2 I/O is the executive's
kernel socket or it does not happen.

Two runs, **one variable: the executive's source.**

| run | OVMX built from | dir |
|---|---|---|
| 1 | this branch (the op-0x12 relay answer) | `run1-fixed/` |
| 2 | `origin/main` @ `fc24fe00`, the shipped behaviour | `run2-base-control/` |

## 1. What the run set out to test, and what it could not

rd vms-4f0's defect is that a third participant is refused and the real VAX
stops driving its interconnect. The hypothesis this branch fixes is the
member-side one: OVMX classified the coordinator's cat-0x01 **op-0x12 RELAY**
in no table, logged it as "an unroutable VMS$VAXcluster frame was received",
and answered nothing — and §4(O.31) measures that relay as the **commit gate**
between the joiner's op-0x02 and the coordinator's op-0x03.

**That path was not exercised on this rig, and the README says so.** On both
runs the third node (OVMXB) sent its op-0x02 to **OVMXA**, not to VAXC,
because at that instant it had a `VMS$VAXcluster` circuit only to OVMXA — the
SIMH VAX's channel to a newly-arrived third node forms tens of seconds later.
So the relay went **OVMXA → VAXC**, and it was the real VAX that answered it
(which it did, in 0.2 ms, both runs). The member-side answer this branch adds
is proven at rung R1 instead, byte-for-byte against a real VAX's own answer
(`tests/cluster/host/fixtures/cm-relay-oracle-*.spec`,
`test_cnxman_join.c::test_4f0_member_answers_the_relay_like_a_real_vax`).

## 2. ⚠ WHAT THE RUN DID FIND: OVMX-as-coordinator BUGCHECKS a real VAX

Both runs, identically. Once OVMXA is a member and OVMXB asks **it** to admit
a third node, OVMXA appoints itself transition coordinator and drives an ADD
transition at the real VAX. The VAX takes a **fatal `CNXMGRERR` bugcheck**
1.3 ms later, dumps memory, halts and reboots.

`run1-fixed/cn3.pcap`, decoded with `cn3dec.py` (t is pcap-relative):

```
 199.7872 OVMXB >OVMXA  cat=01 op=02   the third node asks OVMXA to admit it
 199.7949 OVMXA >VAXC   cat=01 op=12   OVMXA relays it to the other member, epoch 4
 199.7951 VAXC  >OVMXA  cat=81 op=12   the REAL VAX answers in 0.2 ms, epoch 3
 199.7962 OVMXA >OVMXB  cat=01 op=03   OVMXA commits the joiner
 199.8002 OVMXA >VAXC   cat=01 op=05   membership record, epoch 0
 199.8005 OVMXA >VAXC   cat=01 op=09   ADD transition OPEN, epoch 4, bitmap 0x0e
 199.8018 VAXC  >MCAST  mt=b1          the VAX's LAST frame  <-- 1.3 ms later
 ...                                    silence until 240 s (dump + reboot)
```

`run1-fixed/VAXC.console.log`, the real VAX's own console:

```
**** Fatal BUG CHECK, version = V7.3     CNXMGRERR, Error detected by VAXcluster Connection Manager
    Crash CPU: 00        Primary CPU: 00
    Current process = NULL
        PC = 83C3E2A9   PSL= 04080009
**** Starting memory dump, writing dump to unit number 0
**** Memory dump complete, dump written  to unit number 0
?06 HLT INST
```

**`run2-base-control/` is the same thing on the SHIPPED executive**, with the
op-0x12 fix absent (verified: the control initramfs's `vms.ko` contains none
of this branch's strings). Same sequence, same bugcheck:

```
  37.9426 OVMXB >OVMXA  cat=01 op=02 epoch=0
  37.9540 OVMXA >VAXC   cat=01 op=12 epoch=4
  37.9542 VAXC  >OVMXA  cat=81 op=12 epoch=3
  37.9554 OVMXA >OVMXB  cat=01 op=03 epoch=4
  37.9596 OVMXA >VAXC   cat=01 op=05 epoch=0
  37.9596 OVMXA >VAXC   cat=01 op=09 epoch=4 bitmap=0e
```
```
**** Fatal BUG CHECK, version = V7.3     CNXMGRERR, Error detected by VAXcluster Connection Manager
```

**So this branch neither causes nor cures that crash.** It is a pre-existing
defect in the COORDINATOR path (`src/kernel-core/vms_cnxman_coord_fsm.c`),
reproduced here as a single-factor control, and it is what blocks CN=3 on this
rig. Three facts about the frames the real VAX rejected, all readable above:

* the **op-0x05 membership records carry epoch 0** while the transition they
  belong to is epoch 4;
* the **op-0x09 ADD open is at epoch 4** while the real cluster is at epoch 3
  and has just said so in its own 0x81/0x12;
* its **nodemap is `0x0e`** where VAXC's own op-0x09 for the same cluster
  carried `0x06`.

Nothing here says which of those the VAX's connection manager objected to.
That is a separate item, in another lane's file, and nothing in this branch
touches it.

## 3. The by-product: §4(r)'s op-0x12 epoch rule, corrected on the wire

The relay exchange at t=199.79 is a **single-factor experiment nobody had
run**: OVMX relayed at **epoch 4** to a real VAX that was at **epoch 3**.

```
OVMXA->VAXC cat=01 op=12   body[12:16] = 04 00 00 00   body[20:24] = 00 00 00 00
VAXC->OVMXA cat=81 op=12   body[12:16] = 03 00 00 00   body[20:24] = 03 00 00 00
```

§4(r) had recorded "`body[20:24]` = LE u32 copy of the request's
`body[12:16]`". The real VAX put **its own** epoch in **both** fields and
echoed neither. Re-reading the corpus with that question (`relay_epoch.py`)
finds 143 matched pairs, of which only **two** have differing epochs — and both
agree with the VAX here:

```
d94-ctl1.pcap   request epoch 07 -> response body[12:16] = body[20:24] = 06
d94-rej3.pcap   request epoch 10 -> response body[12:16] = body[20:24] = 0f
```

The rule looked like an echo only because in 141 of 143 pairs the two nodes
held the same epoch. The correction is in `docs/cluster-protocol-spec.md`
§4(r) and in `vms_cm_relay_response_build()`.

## 4. What is NOT claimed

* **CN=3 is not achieved.** No console named three members at any point in
  either run; the real VAX bugchecks during the third admission on both the
  fixed and the control executive.
* The member-side op-0x12 answer this branch adds **was not exercised on this
  wire** (see §1). Its proof is rung R1 against a real VAX's captured answer.
* No `SHOW CLUSTER` read-back naming three members exists. `SHOW CLUSTER` on
  OVMXA named OVMXA + 1989 for the CN=2 window of both runs.
* Both runs ran under QEMU **TCG**, not KVM: the pod's device cgroup denies
  `/dev/kvm` to a non-privileged container even with the node's `/dev/kvm`
  hostPath-mounted, and escalating the pod to privileged was not done.

## Files

* `run1-fixed/` — consoles (all three nodes), cap evidence, pcap: this branch.
* `run2-base-control/` — the same, on `origin/main` @ `fc24fe00`.
* `cn3node.sh`, `cn3start.sh` — the in-pod launchers (one node each, detached).
* `cn3dec.py` — the CM-dialogue decoder used above.
* `relay_census.py`, `relay_pairs.py`, `relay_epoch.py` — the reference-corpus
  censuses: how many op-0x12 relays are answered (151 requests, 148 answers;
  the three that are not went to a departed node and to OVMX itself), whether
  the response recipe is a verbatim echo plus the named mutations, and the
  epoch question of §3.

## Reproducing

```
# in a temporary pod on k3s-worker with the vax-lab PVC and NET_ADMIN:
ip link add br4f0 type bridge; ip link set br4f0 up
for t in tapC4f0 tapA4f0 tapB4f0; do ip tuntap add dev $t mode tap
  ip link set $t master br4f0; ip link set $t up; done

# artifacts: docker build -f distro/Dockerfile.bootable -o dist .
# then per node: tools/cluster-web-demo/inject-cluster-config.sh <in> <out> \
#   --scsnode OVMXA --scssystemid 1987 --votes 1 --expected-votes 2 --group 257

bash cn3start.sh cap
bash cn3start.sh C                       # SIMH real V7.3, boot B DUA0
ART_ROOT=<artdir> bash cn3start.sh A     # wait for MEMBER
ART_ROOT=<artdir> bash cn3start.sh B     # the third node
```
