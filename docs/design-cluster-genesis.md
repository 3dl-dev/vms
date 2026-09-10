# Design: Executive OVMX-only cluster GENESIS (vms-3c3)

**Status:** DESIGN — awaiting operator gate (authenticity call). Prerequisite for
vms-1ee (real 2-node OVMX↔OVMX DLM proof).
**Date:** 2026-09-10. **Lane:** cluster.

## The problem: two fresh OVMX executives cannot form a cluster

The cross-node DLM wire is an OVMX-derived byte layout (Rule 8 — VSI does not
publish the SCS lock-message layout; see `docs/compat/facilities/cluster-dlm.yaml`
`wire_format`). A real VAX cannot speak it. So the only way to prove cross-node
DLM (ENQ/GRANT/BLKAST, LVB crossing, remaster, distributed deadlock) *on a real
wire between two real executives* is **OVMX↔OVMX** — two booted OVMX nodes in one
cluster. That is what vms-1ee needs to restore honest `verified` on the four
`cluster-dlm` rows that #1052 downgraded (their multi-node proofs were retired
with the userspace SCSD daemon).

Two booted OVMX executives cannot join each other today. The join *handshake* is
fine — the executive PE/SCS/join FSMs are symmetric and self-driving (heard
multicast HELLO → directed verify `vms_pe_fsm.c` `h_first_sight`; own round-0
START `h_vc_channel_up`; the 0x5b joiner-accept server half
`cnxman_join_connect_req`). The retired `scsd.c` responder-only limitation is
gone. The blocker is **admission**, specifically **cluster-CSID genesis**:

- To reach `VMS_CLUSTER_MEMBER` a node needs a learned **CSID**
  (`generation << 16 | (SCSSYSTEMID & 0x3ff)`). The only setter of
  `local_csid_valid` is `cnxman_club_learn_local_csid()`
  (`src/kernel-core/vms_cnxman_csb.c:732`).
- Its only caller is the **op-0x06 membership-burst** handler
  (`src/kernel-core/vms_cnxman_join_fsm.c:2107`) — a node learns its generation
  *from a peer's membership burst*.
- To emit op-0x06 you must be **coordinator**. The coordinator predicate
  **hard-refuses** without an already-valid CSID
  (`src/kernel-core/vms_cnxman_coord_fsm.c:196`: *"this node has no cluster
  system id; it cannot coordinate a state transition"*), and coordinator role is
  only ever *entered by being told* via an op-0x02 relay (`coord_fsm.h`).
- `cnxman_start_join_or_wait()` (`src/kernel-core/vms_cnxman.c:1794`) discovers
  peers, tries `cnxman_join_drive()`, and otherwise sits `JOINING`
  (VAXCLUSTER=2) or `STANDALONE` (VAXCLUSTER=1). Its comment is explicit:
  *"NOTHING HERE FABRICATES A MEMBERSHIP … This function's strongest output is
  JOINING."*

**The cycle:** two fresh nodes → neither has a CSID → neither can coordinate →
neither emits op-0x06 → neither learns a CSID → both stay NEW/JOINING forever.
There is no ioctl/test/lab seam that seeds a CSID (`cluster_api.c` only *reads*
`local_csid`).

This is **honest, not an overclaim.** The executive never fabricates membership;
`connection-manager.yaml` makes no `verified` OVMX-only-cluster claim
(`$join`/`$boot-join` = `implemented`, `$real-vax-join` = `absent`). It is a
missing *capability*, not an authenticity gap.

### Why it deadlocks now — the removed LARP

The predecessor of this stack **defaulted the local CSID to 1** from an insmod
parameter and "became a phantom cluster of one" (the comment at
`vms_cnxman_coord_fsm.c:196` records this). #1052 removed that under INV-6: an
unconditional default CSID is a fabricated membership. Correctly removed — but it
left *no* legitimate founding path in its place.

## The VMS-authentic answer: quorum-grounded founding

Real VMScluster **does** form from nothing: the first node up that can satisfy
quorum **by its own votes** forms a single-node cluster as the founding member,
assigns cluster generation 1, and becomes coordinator. Subsequent nodes join
through it. This is the documented connection-manager formation algorithm
(*OpenVMS Cluster Systems*, ch. 7), not a fabrication.

The grounding is already in the executive:

- Per-node SYSGEN params (`src/kernel-core/vms_cluster.h:139-151`): `votes`
  (VOTES — **defaults to 0**, "0 first, design D-10"), `expected_votes`
  (EXPECTED_VOTES), `qdskvotes` (QDSKVOTES), `vaxcluster` (0/1/2).
- Quorum math (`src/kernel-core/vms_cnxman_quorum.c`): `CEVOTES =
  max{EXPECTED_VOTES, ΣVOTES, old CEVOTES}` (p. 7-6); `quorum = (CEVOTES+2)/2`;
  `quorum_lost = present_votes < quorum`.

**Proposed genesis predicate (the INV-6-clean gate):** a booting node forms
generation-1 as founding coordinator **iff** it is a voting node whose own votes
already satisfy quorum on their own — i.e. `vaxcluster >= 1` **and** `votes > 0`
**and** `votes >= (max{EXPECTED_VOTES, votes} + 2) / 2`. Then and only then:

1. mint `csid = (1u << 16) | (SCSSYSTEMID & 0x3ff)` — generation 1;
2. `cnxman_club_learn_local_csid(club, csid)` for *this* node;
3. enter coordinator role for a cluster-of-one (self-nodemap) and drive the
   existing phase2 commit, which sets `VMS_CLUSTER_MEMBER` from a **real**
   membership record naming this node's own SCSSYSTEMID (the E30 invariant the
   current code already enforces).

A node with `votes == 0`, or `votes < quorum`, still stays `JOINING`/`STANDALONE`
exactly as today — it genuinely cannot form a cluster, and says so honestly. This
is the crucial difference from the removed LARP: **the old code minted a CSID
unconditionally; this mints one only when quorum is authentically met by the
node's own configured votes.** No phantom cluster: a VOTES=0 node never founds.

The second OVMX node (VOTES=0, or non-quorum) boots, discovers the founder, and
joins the normal way — the founder is now a real member that can be told to
coordinate (op-0x02) and emits the op-0x06 the joiner learns its CSID from. The
whole downstream stack (VC, SYSAP, DLM) is already symmetric, so the DLM proof
then runs OVMX↔OVMX with no further join work.

## The operator gate (why this is not just mine to build)

Genesis is a **new externally-visible capability** ("OVMX forms its own
clusters") and its mint touches the authenticity posture that #1052 tightened.
The decision reserved to Baron:

1. **Is quorum-grounded gen-1 founding the sanctioned mechanism?** (Recommended —
   it is the documented VMS algorithm and INV-6-clean by the votes gate above.)
2. **Generation source.** Gen 1 for a from-nothing founder is unambiguous. A
   *re-formation* after total cluster loss should advance the generation; for
   1.0's OVMX-only lab use, gen-1-from-cold is sufficient. Flag if reformation
   generation must be persisted.
3. **Scope for 1.0.** Is OVMX-only clustering a 1.0 headline, or is it acceptable
   to accept `implemented` on the four cluster-dlm rows for 1.0 and schedule
   genesis + the 2-node DLM proof as a post-1.0 epic? (vms-1ee already carries
   this accept-vs-rebuild decision; this note supplies the real cost.)

## If sanctioned: implementation + test plan

- **Code:** a `cnxman_try_genesis()` guarded by the votes predicate, called from
  `cnxman_start_join_or_wait()` *after* `cnxman_join_drive()` returns false and
  *before* the JOINING/STANDALONE fallthrough (join an existing cluster first;
  only found if there is genuinely no one to join). Reuses
  `cnxman_club_learn_local_csid` + the existing coordinator/phase2 path — no new
  wire ops, no new frame types.
- **Honest-omission negctl:** a VOTES=0 (or sub-quorum) node MUST still refuse to
  found — assert it stays JOINING/STANDALONE and never mints a CSID. This is the
  anti-LARP teeth (the negative control that would have caught the removed
  default).
- **Host unit:** genesis predicate truth table over (votes, expected_votes) in
  `tests/cluster/host/` (pure, CI-cheap).
- **The 2-node proof (vms-1ee):** hermetic QEMU `socket` mcast rig
  (run_dlm_h1_derisk.sh pattern) on the k3s-worker KVM pod (worker exposes
  /dev/kvm+vmx; workshop is OOM-barred by operator directive). Node A: VOTES=1,
  EXPECTED_VOTES=1 → founds gen-1. Node B: VOTES=0 → joins A. Then drive the DLM
  rungs (ENQ/GRANT/BLKAST, H8/H9 LVB crossing, H10a/b remaster, e84
  dir-ownership, ec75 deadlock) with every result read from **real executive
  state on both nodes** (INV-6) — restoring the retired `verified`.

## References

- `docs/compat/facilities/cluster-dlm.yaml` (the four downgraded rows + wire_format)
- `docs/compat/facilities/connection-manager.yaml` (`$join`, `$boot-join`, `$real-vax-join`)
- `src/kernel-core/vms_cnxman.c`, `vms_cnxman_coord_fsm.c`, `vms_cnxman_csb.c`,
  `vms_cnxman_join_fsm.c`, `vms_cnxman_quorum.c`, `vms_cluster.h`
- vms-1ee (the DLM proof this unblocks), vms-3c3 (this item)
- retired pre-pivot member-role items (do NOT resurrect — SCSD.EXE deleted,
  `test_no_scsd_image.sh` forbids it): vms-ae14, vms-f3e, vms-d60, vms-45c, vms-164d
