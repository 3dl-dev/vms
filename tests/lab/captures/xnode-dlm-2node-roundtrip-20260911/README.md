# xnode DLM 2-node FULL ROUND-TRIP proof (2026-09-11)

rd vms-c72 / vms-94c. Re-run of the cross-node DLM genesis rig
(`tests/qemu/Dockerfile.cluster-genesis-2node`,
`tests/qemu/k8s-cluster-genesis-rig.yaml`, `RIG_MODE=xnode`) at the tip of
`work/vms-c72-receive-arm` (PR #1181, commit `b2fa15de`), which adds the
receive arm: inbound op-0x03 $DEQ -> `vms_lock_dlm_master_serve` (real
master-side release), inbound op-0x04 BLKAST -> `dlm_req_fsm_blkast_body` ->
`vms_lock_dlm_proxy_blkast_recv` (real user-mode AST on the remote holder).
Built and run image: `192.168.2.43:30500/ovmx-genesis-rig:26`. Pod
`ovmx-genesis-rig` in ns `ovmx-lab`, scheduled on `k3s-worker` (`/dev/kvm`),
exit code 0.

The prior run (#1180, image `:25`, built from `main` without the receive
arm) showed emit only: `blkasts_received=0`. This run flips that.

## Result: round-trip CLOSED

From `run.log` (`RIG-*-DLM-EMIT`/`RIG-*-DLM-RECV` at `survival`, read back
out of each node's own executive via `VMS_IOCTL_CLUSTER_DIAG_DLM`,
`0xC090566E`):

```
A: releases_sent=2 releases_no_wire_op=1 blkasts_sent=1 blkasts_received=1 blkasts_delivered=1 unparsed=0
   releases_received=1 releases_refused=0
B: releases_sent=1 releases_no_wire_op=1 blkasts_sent=1 blkasts_received=1 blkasts_delivered=1 unparsed=0
   releases_received=2 releases_refused=0
```

- **op-03 RECEIVED (not declined-unparsed):** `releases_received=1` (A),
  `releases_received=2` (B), both with `releases_refused=0`. Traced to
  source (`src/kernel-core/vms_dlm_scs.c:dlm_arm_serve_deq`):
  `releases_received++` fires only when `vms_lock_dlm_master_serve()`
  returns outcome `VMS_DLM_MASTER_RELEASED` — a real call into the engine's
  master-side LKB table, not a counter bump. That is the master's own copy
  of the peer's lock being released.
- **op-04 DELIVERED (real AST, not unparsed):** `blkasts_received=1` and
  `blkasts_delivered=1` on both nodes. Traced to source
  (`vms_dlm_scs.c:dlm_arm_deliver_blkast` ->
  `dlm_req_fsm_blkast_body` -> `f->blkasts_delivered++` in
  `vms_dlm_scs_fsm.c`, which calls the engine's
  `vms_lock_dlm_proxy_blkast_recv` — a real user-mode AST fire on the
  holder, not a template reply).
- **emit still fires:** `releases_sent` = 2 (A) + 1 (B) = 3,
  `blkasts_sent` = 1 (A) + 1 (B) = 2. Matches the wire: `scan_dlm_wire.py`
  on each node's own reconstructed pcap: `deq=1` local-observed +
  cross-counted totals reconcile to `op-0x03 deq frames=3`,
  `op-0x04 blkast frames=2` (see the run's own reconciliation line and
  `nodeA.pcap`/`nodeB.pcap` here, independently re-scanned after copy-out —
  identical counts).
- **never-crash:** `RIG-A-FINAL role=founder member=1 state=MEMBER cn=2
  ... projections=agree`, `RIG-B-FINAL role=joiner member=1 state=MEMBER
  cn=2 ... projections=agree`, both AFTER the DLM phase. `dmesg` on both
  nodes (embedded in `run.log`, also `nodeA.console.log`/
  `nodeB.console.log`): no BUG/Oops/Call Trace/panic.
- **rig's own flipped verdict:** `CROSS-NODE DLM PROOF PASSED (rd vms-94c)`
  printed by the rig itself; pod exit code 0.

## One honestly-counted non-fabrication note

Each node shows `releases_no_wire_op=1` in addition to its `releases_sent`.
This is the FSM's existing honest-gate counter
(`vms_dlm_scs_fsm.c:h_post_deq`, `dq_may_transmit`) — a release attempt
that a gate (e.g. the all-OVMX shape gate, or an unresolved directory
route) declined to put on the wire, logged rather than silently dropped or
faked as sent. It does not affect the round-trip count reconciliation
above (`releases_sent` sums to the wire's `deq=3`, matching exactly).

## Gap noted, not fixed (scope: re-run only, no rig-script edits)

The rig prints `GET_RESMASTER` reads only for the resource each node is
the REQUESTER on (the name the PEER masters), never a before/after
`GET_RESMASTER` on the resource THIS node masters FOR the peer. So there
is no direct printed "master's own grant count dropped after the DEQ"
line in this log; the "master copy released" claim rests on the source
trace above (`vms_lock_dlm_master_serve` outcome
`VMS_DLM_MASTER_RELEASED`), not an explicit resmaster snapshot delta. A
minimal rig addition to print `GET_RESMASTER` on the peer-owned resource
name from the master's own side would close that directly; left to the
branch owner since the instructions were to re-run, not edit the rig.

## Files

- `run.log` — full pod stdout (both nodes' ttyS1 markers, embedded dmesg,
  wire reconciliation, verdict).
- `nodeA.ttyS1.log` / `nodeB.ttyS1.log` — each node's raw ttyS1 stream.
- `nodeA.console.log` / `nodeB.console.log` — each node's raw serial
  console (includes dmesg).
- `nodeA.pcap` / `nodeB.pcap` — each node's own passive reconstructed
  capture (`sca_l2probe`, receive-only). Independently re-verified with
  `python3 tests/qemu/scan_dlm_wire.py nodeA.pcap nodeB.pcap` after
  copy-out: `nodeA.pcap: sca_frames=477 blkast=1 deq=1 enq=3 enq-resp=2`,
  `nodeB.pcap: sca_frames=476 blkast=1 deq=2 enq=4 enq-resp=1` — identical
  to the in-run scan.

Build: `docker build -f tests/qemu/Dockerfile.cluster-genesis-2node -t
192.168.2.43:30500/ovmx-genesis-rig:26 .` at branch tip `b2fa15de`
(`work/vms-c72-receive-arm`). Run:
`kubectl -n ovmx-lab apply -f tests/qemu/k8s-cluster-genesis-rig.yaml`
(image bumped to `:26`), `logs -f`, `delete pod`. No vaxlab-* pod was
touched; the pcaps/consoles were pulled off the shared `vax-lab-pvc` via a
short-lived `busybox` reader pod, also deleted after copy.
