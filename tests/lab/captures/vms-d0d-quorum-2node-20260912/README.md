# vms-d0d — the JOINER's own quorum arithmetic, 2-node live run (2026-09-12)

`genesis-2node-quorum-CLEAN-run.log` is the full console of one run of the
2-node cluster GENESIS rig (`tests/qemu/run_cluster_genesis_2node.sh`,
`RIG_MODE=proof`) on `k3s-worker` under KVM, image
`192.168.2.43:30500/ovmx-genesis-rig:37` built from this branch. Pod exit 0,
"GENESIS 2-NODE PROOF PASSED", neither node crashed.

Both guests insmod the real executive and issue STARTUP.EXE's two cluster
ioctls; every number below is READ BACK out of `/dev/vms`
(`VMS_IOCTL_CLUSTER_DIAG_CSB` rows CLUB and CSB, `VMS_IOCTL_CLUSTER_GETSYI`).
The rig asserts nothing about membership or quorum — it prints what the
executive holds.

Rig SYSGEN (the only configuration): A `VOTES=1 EXPECTED_VOTES=1`,
B `VOTES=0 EXPECTED_VOTES=1`, both `VAXCLUSTER=2`.

## The state-delta this run measures

Node B is the JOINER. Before this change its Phase 2 had no coordinator
proposal to copy, so nothing on its path ever applied the quorum algorithm:

    BEFORE (same rig, tests/lab/captures/vms-727-lvb-2node-20260911/)
      RIG-B-FINAL role=joiner member=1 state=MEMBER csid=0x00010002 cn=2 \
                  quorum=0 cevotes=0 epoch=2 projections=agree

    AFTER (this run)
      RIG-B-FINAL role=joiner member=1 state=MEMBER csid=0x00010002 cn=2 \
                  quorum=1 cevotes=1 qlost=0 epoch=2 projections=agree
      RIG-B-GETSYI member=1 nodes=2 votes=0 quorum=1 csid=0x00010002

## Where B's number comes from (INV-6: it is not B's own opinion)

The `RIG-*-CSBQ` rows are this run's new readout: each CSB's own VOTES and the
executive's `votes_valid` flag, which only a real op-0x01 PARAMS record from
that peer ever sets.

    t=1s  RIG-B-CLUB  state=JOINING(2) nodes=0 cevotes=0 quorum=0 qlost=0
          RIG-B-CSBQ  i=1 csid=- votes=1 votes_valid=1 selected=0 state=3(OPEN)

B has ALREADY learned node A's real VOTES off the wire while still JOINING —
and publishes NO quorum for them, because it is not a member yet (the honest
refusal).

    t=2s  RIG-B-CLUB  state=MEMBER(3) nodes=2 cevotes=1 quorum=1 qlost=0 expected=1
          RIG-B-CSBQ  i=0 csid=0x00010002 votes=0 votes_valid=1 selected=1 state=9(LOCAL)
          RIG-B-CSBQ  i=1 csid=0x00010001 votes=1 votes_valid=1 selected=1 state=3(OPEN)

At the commit the arithmetic runs over B's own CSB table: SUM VOTES = 0 (B) +
1 (A, learned) = 1, QUORUM = (1+2)/2 = 1.

`qlost=0` is the field that makes A's learned vote load-bearing rather than
merely consistent: PRESENT votes must be >= QUORUM, B's own VOTES are 0, so the
only vote that can hold quorum here is A's — counted because its CSB is
`votes_valid=1` AND `state=OPEN`. Had B not learned it, A's votes would have
contributed nothing (unknown, never assumed zero) and B would have reported
`qlost=1`.

Node A (the founder) is unchanged: `cevotes=1 quorum=1 qlost=0`, CN=2, and the
two nodes now agree on the cluster's quorum figures.

## The teeth (`genesis-2node-negctl-run.log`)

The same image in `RIG_MODE=negctl` (VOTES=0 on BOTH nodes, so nobody may
found). Both nodes still exchange PARAMS and still see each other's CSBs, and
both report:

    RIG-B-FINAL role=none member=0 state=JOINING csid=- cn=0 quorum=0 cevotes=0 qlost=0

Nothing was computed, because neither node is a member — the recompute's own
gate, live. A quorum that appeared here would be the local-only fabrication
INV-6 forbids.
