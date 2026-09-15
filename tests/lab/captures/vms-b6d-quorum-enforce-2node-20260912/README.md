# vms-b6d — QUORUM ENFORCEMENT: the executive stalls and resumes, 2-node live run (2026-09-12)

Three runs of the 2-node cluster rig (`tests/qemu/run_cluster_genesis_2node.sh`)
on `k3s-worker` under KVM, image `192.168.2.43:30500/ovmx-genesis-rig:b6d3`
built from this branch (`work/vms-b6d-quorum-enforce`, off d0d-inclusive main
`0045a3ba`). All three pods exited 0; no console shows a panic or a bugcheck.

Both guests insmod the real executive and issue STARTUP.EXE's two cluster
ioctls. Every quorum figure and every lock state below was READ BACK out of
`/dev/vms` (`VMS_IOCTL_CLUSTER_DIAG_CSB` rows CLUB/CSB, `VMS_IOCTL_GETLKI`,
`VMS_IOCTL_GET_RESMASTER`). The rig asserts nothing — it prints what the
executive held.

Rig SYSGEN: A `VOTES=1 EXPECTED_VOTES=1`, B `VOTES=0 EXPECTED_VOTES=1`, both
`VAXCLUSTER=2`, `RECNXINTERVAL=45`.

---

## 1. `qhang-CLEAN-run.log` — THE PROOF (`RIG_MODE=qhang`)

After CN=2 settles the rig PARTITIONS its own segment: `segment_relay.py` sits
in the middle of the two guests' netdev wire and DROPS whole frames for 45
seconds, then forwards again. Neither guest is told; each discovers the loss the
way a real system does, by its channel timing out (20 s, `vms_pe_fsm.h`).

    RELAY CUT  at t=101.4s (frames are dropped from here; the guests are told nothing)
    RELAY HEAL at t=146.5s (forwarded=344 dropped=65 during the cut)

### Node B (VOTES=0 — its quorum is A's learned vote, rd vms-d0d): STALL → RESUME

    RIG-B-QH-PICK  res=QH_B0 master_csid=0x00010001 is_local_master=0   <- the PEER masters it: skipped
    RIG-B-QH-PICK  res=QH_B1 master_csid=0x00010002 is_local_master=1   <- THIS node's own grant decision

    RIG-B-QH-CLUB  at=before  nodes=2 cevotes=1 quorum=1 qlost=0
    RIG-B-QH-ENQ   at=before  res=QH_B1 status=1 lkid=0x00000004 granted_mode=5   <- EX GRANTED

    RIG-B-QH-LOSS  observed=1 polls=61                                  <- the cut reached its executive
    RIG-B-QH-CLUB  at=during  nodes=2 cevotes=1 quorum=1 qlost=1
    RIG-B-QH-DEQ   at=during  lkid=0x00000004 status=1                  <- a RELEASE still works
    RIG-B-QH-ENQ   at=during  res=QH_B1 status=1 lkid=0x00000006 granted_mode=0   <- THE STALL

    RIG-B-QH-REGAIN observed=1 polls=51
    RIG-B-QH-CLUB  at=after   nodes=2 cevotes=1 quorum=1 qlost=0
    RIG-B-QH-AFTER res=QH_B1 lkid=0x00000006 granted_mode=5 polls=2     <- THE SAME lock, granted

`status=1` is `SS$_NORMAL` at every step: the stall is a STALL, never an error
return (Davis p. 7-4 — the system "blocks activity and waits for quorum to be
regained"). `granted_mode=0` on a resource whose only other holder is this same
process's NL anchor means nothing but quorum can explain the wait; `lkid` is
unchanged across the regain, so what completed is the ORIGINAL request, never a
re-issued one.

B's own CSB rows are where its quorum comes from, and they are unchanged by the
cut apart from reachability:

    RIG-B-CSBQ i=0 csid=0x00010002 votes=0 votes_valid=1 selected=1 member=1 state=9(LOCAL)
    RIG-B-CSBQ i=1 csid=0x00010001 votes=1 votes_valid=1 selected=1 member=1 state=3(OPEN)

The executive's own OPA0: lines, from node B's dmesg in the same log:

    [   65.763518] %CNXMAN, quorum lost, blocking activity
    [   91.403272] %CNXMAN, quorum regained, resuming activity

### Node A (VOTES=1 — still meets QUORUM=1 alone): THE IN-RUN CONTROL

    RIG-A-QH-CLUB at=during nodes=2 cevotes=1 quorum=1 qlost=0
    RIG-A-QH-ENQ  at=during res=QH_A0 status=1 lkid=0x00000005 granted_mode=5

Same partition, same instant, same request shape — GRANTED AT ONCE. This is what
separates "stalls on quorum loss" from "stalls whenever a peer goes away", and a
build that froze on the departure rather than on the arithmetic fails here.

Both nodes finish `member=1 cn=2` (A founder `0x00010001`, B joiner
`0x00010002`), projections agreeing, neither console panicked.

---

## 2. `proof-mode-control-run.log` — no partition, no stall (`RIG_MODE=proof`)

The ordinary CN=2 genesis run on the SAME image. A founds, B joins, and both
nodes' `$ENQ` is granted with a real lock id (`RIG-*-DLM-ENQ status=1
lkid=0x00000001`). Nothing stalls anywhere in a whole normal run — including
every join transient, which is the window the enforcement gate exists to keep
out of (`GENESIS 2-NODE PROOF PASSED`).

## 3. `negctl-genesis-run.log` — the genesis teeth, unchanged (`RIG_MODE=negctl`)

`VOTES=0` on BOTH nodes: nobody may found, so nobody reaches MEMBER or holds a
CSID (`role=none member=0 cn=0 csid=-`) — and both nodes still LOCK freely
(`RIG-*-DLM-ENQ status=1 lkid=0x00000001`). A node with no cluster has no quorum
to lose, and the gate is not installed on it at all: the honest degradation
`vms_dlm_scs_stop()` already gave the wire path.

---

## The defect this run caught (and the fix it drove)

The FIRST run of this scenario (same code, earlier image) produced the stall and
then failed the resume: `RIG-B-QH-REGAIN observed=1` followed by
`RIG-B-QH-AFTER granted_mode=0`. The connection manager measured its edge as a
before/after pair taken around its OWN recompute — and on the heal it was the
JOINER's op-01 PARAMS-learn path (rd vms-d0d) that cleared `quorum_lost` between
two beats, so the comparator saw no change, announced nothing, and left a
stalled request stalled on a node that had quorum again. The edge is now
LEVEL-TRIGGERED against what was last ANNOUNCED (`quorum_hang_announced` in
`struct vms_cnxman`), which cannot miss it whichever path did the arithmetic.
Only a two-node run with a real partition could see that; the host tests could
not.
