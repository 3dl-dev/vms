# Cluster build — cross-item integration escalations (running list)

Contract questions and cross-item couplings surfaced by landed FC-P items. Any
agent implementing a **glue** item (P0.9, P1.3, P2.x, P3.8) or a coordinator/
barrier item MUST read this before wiring. Each entry: who raised it, what it
affects, and the current disposition.

## RESOLVED rulings

### E1. Seam buffer semantics → BODY-LEVEL (Fable ruling, design §3.2.4)
**RULED: body-level. Each layer owns exactly its own header.** A SYSAP hands SCS
a **132-byte body**; a SYSAP that fills `send_seq` is the same error as a daemon
filling a lock id. Byte ownership of the 204-byte class (frame-absolute):
- **0–55 → port** (`vms_pe`): Ethernet, SCA header, `recv_ack`/`send_seq` @32–35,
  VC mirrors @36–55. msgtype @30 set by the PORT from a service kind SCS passes
  down (the `0x5b/0x4b` phase rule is SCS knowledge, never a SYSAP byte).
- **56–71 → SCS** (`scs_send_msg` from the CDT): inner length, format, MTYPE,
  credit, dst/src Con.ID.
- **72–79 → CNXMAN** via ONE pure stamper `cnxman_envelope_stamp(csb, body,
  is_response)` on the CSB's dialogue counters (send_msg/ack_msg/txn/token);
  responses echo txn/token; the DLM cat-02 arm uses the same stamper and never
  writes body[0:8].
- **80–203 → the emitting FSM/role** (cat/op/payload).
- **Seam:** `cnxman_ops.send(dst_csid, body, 132)` → glue resolves the CSB's
  `VMS$VAXcluster` CDT → `scs_send_msg`; `respond(body,len)` → same on the
  request's CDT. `scs_send_msg(local_conid, body, len)` copies body to a pool
  `exec_lanbuf_t` @72, fills 56–71, debits credit, `pe_send_msg(...svc)`.
  `pe_send_msg` fills 0–55, assigns seq, keeps the unacked ring (retransmit =
  same bytes/seq). Receive: no copy/strip — deliver whole frame +
  `vms_frame_info{scs_off=56, body_off=72}`.
- **Codec** organized by owner: `codec_hello`/`codec_vc` (0–55), `codec_scs`
  (56–71), `codec_cm`/`codec_dlm`/`codec_mscp` (bodies, `body[]`-relative). A
  `vms_frame_compose(link,scs,body)` exists for tests/simulator ONLY.
- **Binds:** FC-P0.9, FC-P1.3, FC-P2.2, FC-P2.4, FC-P3.8 build to this contract.
- **CONFORMANCE RETROFIT = FC-P3.15** (Sonnet-tier). Fable scoped it to FC-P3.5,
  but the integrator WIDENS it to cover **every CNXMAN frame-emitter landed
  frame-level**: FC-P3.5 barrier AND FC-P3.12 coordinator (dispatched against the
  superseded frame-level P3.5). One body-level pass: introduce
  `cnxman_envelope_stamp`; the barrier's + coordinator's call sites emit bodies
  via `ops->send/respond` after the stamper; delete
  `cnxman_barrier_link_ops.next_out`; demote `vms_cm_link` to a test-only composer;
  fixtures become `specimen[72:204]` slices (byte-exact assertions hold on the
  body). R1 must prove NO CNXMAN TU writes below body offset 0.
  **DISPATCH FC-P3.15 ONLY AFTER FC-P3.12 lands** (else it races the coordinator
  on the same files).

## LAB-lane inputs needed (capture/oracle — the lab owns these)

### E12. Decoded `vax3-2to3-established-join` capture for the replay oracle (raised by FC-P1.5)
FC-P1.5's replay driver is generic but currently runs on the VAX2→VAX1
formation-window fixtures in-tree (`hello-directed-vax2-to-vax1.spec` +
`scs-start-vax2-config-round0.spec`). The plan's named
`vax3-2to3-established-join-20260730.pcap` (a node joining an ESTABLISHED
2-node cluster — the richest join specimen) is lab-host-only (`~/vax/` not
present here) with no decoded fixture in-tree. **Lab lane:** clean-room decode
that capture to a `.spec` fixture and swap it into `test_replay.c`'s input list
(no driver change needed). This also feeds E8 (the op-06 MEMBERSHIP record layout
that blocks P3.3 CSID-learning may be isolable from this same established-join
capture).

## RESOLVED / carried couplings

### E2. `enum cnxman_event` has no op-0x0f cell (raised by FC-P3.5)
op-0x0f (the class-0x03 extra step, a real participant obligation) has no
enumerator in the frozen `enum cnxman_event`. FC-P3.5 routed it through a
documented one-opcode auxiliary echo OUTSIDE the `[state][event]` table.
- **Disposition:** FC-P3.12 (coordinator) decides — if it needs op-0f in the
  table, add an enumerator to `vms_cnxman.h` (additive) and update the barrier's
  aux path to match. Carried into the P3.12 dispatch.

### E3. `club->proposed_valid` sets the barrier's proposed→effective quorum copy (raised by FC-P3.5)
The barrier copies proposed→effective quorum cells ONLY if `club->proposed_valid`;
it never sets that flag (asserting quorum 0 would be a fabricated quorum).
- **Status:** FC-P3.7 landed `votes_valid` (per-CSB) but did NOT set
  `proposed_valid` (CLUB-level). So the barrier's effective-quorum copy currently
  never fires. This is acceptable while quorum is tracking-only, but **FC-P3.8
  (or whichever item first drives a real commit) MUST set `proposed_valid` when it
  fills the proposed cells**, or effective quorum stays stale. Not blocking today.

### E4. Codec objects are wired into the kernel module lists by their first FSM consumer
FC-P0.8 wired `vms_cluster_codec.o`/`_hello.o` and FC-P3.5 wired `_cm.o` into
`src/kernel/Makefile`, the distro Kbuild, `src/kernel-netbsd/Makefile`, and the
two `tools/cross-vax/build-*-vax.sh` SRCS — because their FSMs cannot link
without them. A codec TU with **no in-module consumer yet** (e.g. FC-P6.2's
`vms_cluster_codec_mscp`) is wired into the host `tests/cluster/host/CMakeLists.txt`
ONLY, and is added to the kernel lists by the first item that consumes it
in-module (e.g. FC-P3.4/P6.3). **Do not re-add a codec object already present**
(dedup on merge — the integrator has hit this on every FSM merge).

### E10. VC gap-break under loss → RULED a fidelity bug; fix = FC-P1.9 go-back-N (Fable, design §3.2.5)
**RULED: break-on-first-gap is a FIDELITY BUG.** p. 2-31 governs the delivery
guarantee + its consequence (if the port can't satisfy order/delivery, the VC and
every connection on it break) — NOT the detection mechanism. The port satisfies
the guarantee under loss by RETRANSMISSION (wire evidence: 0x7b retransmit
msgtype, retransmit reuses send_seq §4(L), 506 dup/retransmit frames, §4(k) ~25-retry
ladder). The "0 gaps in 321,599" census was a lossless SIMH bridge — can't
distinguish "never tolerates a gap" from "LAN never lost one."
**Faithful model = go-back-N, cumulative acks, receive window = 1:**
- Receiver on a gap: discard the frame, do NOT advance recv_seq, count `rx_gaps`,
  immediately re-send the cumulative ack — NO break.
- Sender: ack-timeout retransmit from the oldest unacked ring entry onward (same
  bytes, same seq, retransmit msgtype), bounded ladder seeded from §4(k) (labeled
  OVMX design values).
- Break ONLY on ladder exhaustion (`PE_VC_DOWN_RETRANSMIT_EXHAUSTED`, NEW),
  `TIMVCFAIL`, or listen timeout. **`PE_VC_DOWN_SEQ_GAP` deleted.** Silence
  detectors stay.
**FC-P2.2 contract (SCS does NOT hide a VC break):** port retransmission is
invisible to SCS (credit spent once at scs_send_msg). On `vc_down(sysid, reason)`:
every CDT on that SB → CLOSED (path-lost), ledgers discarded, pending sends fail
`SS$_PATHLOST`, each SYSAP's `disconnected()` called. SCS never retries across a
break or re-opens itself — CNXMAN's `recnx_fsm` (P3.6) is the SYSAP that reconnects
(§4(aa)). So the CDT ladder needs no "suspended" state.
- **FIX = FC-P1.9** (O5, blocked-by P1.2/P1.4): receiver discard+re-ack; sender
  ack-timeout ladder + exhaustion break; `vc_down` raised through `pe_ops`. R2:
  10% loss + 48 pipelined → all delivered in order, 0 breaks, retransmits>0;
  100% one-way loss → exactly one break (RETRANSMIT_EXHAUSTED) then re-form + SCS
  `disconnected` on every CDT.
- **⚠ FC-P1.9 COLLIDES with FC-P1.3** (both edit vms_pe_fsm.c/vms_pe.h) — dispatch
  FC-P1.9 only AFTER FC-P1.3 integrates. **FC-P2.2 blocked-by FC-P1.9.**

### E11. No pure `pe_fsm_project` — sim reads pe_fsm counters directly (raised by FC-P1.4 → FC-P1.6)
The frozen port view (`struct vms_pe_view`) is filled only by `vms_pe_snapshot()`
in the glue (`vms_pe.c`, not linked at R2). FC-P1.4's `sim_dump.c` reads the
public `struct pe_fsm` counters directly (documented in its header). If FC-P1.6
adds a pure `pe_fsm_project(f, struct vms_pe_view*)`, switch the sim's
`dump_port()` to it. Minor; no ruling needed.

### E9. P1.2's port send API is frame-level (below SCS) — the glue bridges it to the body-level seam (raised by FC-P1.2 → owned by FC-P1.3)
FC-P1.2 implemented the PORT primitive `pe_vc_send_frame(f, dst, frame, len)`:
the owning layer hands down a complete SCS frame and the port stamps the
circuit's sequence into abs 32–55 (+ `vms_scs_seq_stamp`/`_mark_retransmit` in
`codec_vc`). This is NOT in tension with E1's body-level ruling — it is the
**SCS→port boundary** (below SCS), whereas E1/§3.2.4 governs the **SYSAP→SCS
boundary** (the 132-byte body). The full chain: `cnxman_ops.send(body,132)` →
`scs_send_msg` [copy body@72, fill 56–71] → `pe_send_msg`/`pe_vc_send_frame`
[stamp 0–55, seq]. FC-P1.2 kept a body-taking `pe_send_msg` as a stub.
- **FC-P1.3 owns the bridge:** wire `scs_send_msg` to call P1.2's
  `pe_vc_send_frame`, reconcile the `pe_send_msg`(body) vs `pe_vc_send_frame`(frame)
  naming into one coherent SCS-glue surface, and confirm with P2.2.
- **DISPATCH FC-P1.3 AFTER FC-P3.15 lands** (FC-P3.15 settles the body-level
  emitters + `cnxman_envelope_stamp` + demotes `vms_cm_link`; P1.3 builds on that).
- P1.2 also flagged (honest, counted, not faked): undecoded frame classes are
  **acked-but-unrouted** (`vc_rx_undelivered`) — FC-P2.1 grounding more classes
  drives it to zero; a circuit that reached OPEN without a credit-window body
  refuses every send (INV-6, no invented window); retransmit cadence
  TIMVCFAIL/8 is labelled OVMX's choice (no captured figure).

**RESOLVED by FC-P1.3.** The bridge landed as `pe_vc_send_msg`/`pe_vc_send_dg`
(`vms_pe_fsm.h` §8c, `vms_pe_fsm.c`) — pure, `struct pe_fsm *`-level siblings
of `pe_vc_send_frame`/`pe_vc_addr`, which is where the R1 host-test ladder
(`cluster_host_pe`, links only `vms_pe_fsm.c`) can actually reach them.
`vms_pe.h`'s own `pe_send_msg`/`pe_send_dg`/`pe_set_upper` (the frozen,
`struct vms_pe *`-level glue surface FC-P0.1 named) are NOT redefined here —
`struct vms_pe` is undefined and documented "private to vms_pe.c", and
`struct pe_fsm`-typed functions cannot share those names in the same TU
without an ODR conflict (`vms_pe_fsm.h` already includes `vms_pe.h`). FC-P0.9
(whichever item finishes `struct vms_pe`) implements the frozen glue names as
thin one-line wrappers: `pe_send_msg(pe, ...) { return pe_vc_send_msg(&pe->fsm,
...); }`. `pe_send_msg`'s own doc comment in `vms_pe.h` now names this.
- **Contract:** `body`/`len` is exactly `PE_SEND_BODY_LEN` (148) bytes —
  SCS's already-built abs 56-71 envelope (inner length, format, MTYPE,
  credit, the Con.ID pair) plus the 132-byte SYSAP body, matching what
  `scs_send_msg` will hand down once FC-P2.2 lands (confirmed against
  `vms_scs.h`'s `scs_ops.send` shape, which already threads `dst_conid`
  alongside a body). `pe_vc_send_msg` builds abs 0-35 (addressing +
  envelope), leaves abs 36-55 an EXPLICIT ZERO (spec sec 4(d)'s mirror span
  has no generic-message builder yet — this is the interim design SS3.2.4
  itself names, not a new gap), splices `body` at abs 56, fixes up the SCA
  length field (new: `vms_scs_seq_envelope_fixup_len`, `codec_vc.{h,c}`,
  the same rewrite-after-append pattern `vms_hello_build_padded` already
  uses), and hands the assembled frame to `pe_vc_send_frame`.
- **`dst_conid` is not written to the wire** — it already rides inside
  `body` at the position SCS put it; the parameter exists for the port's own
  bookkeeping and for symmetry with `pe_upper_ops.message`'s (from,
  dst_conid) receive shape. A test locks this in (`test_pe_send.c`).
- **`pe_vc_send_dg`** does not consume a `send_seq`, does not enter the
  unacked ring, and is never retransmitted; it stamps `send_seq=0` (spec
  sec 4(h)(3)/(4)'s own "no sequence" value) since sec 4(h)(1c)/(1d)
  REFUTES 0x4b/0x5b as a message-vs-datagram wire discriminator and no
  grounded alternative exists — labelled as OVMX's own choice (Rule 8), not
  a captured VMS behaviour.
- **The (SB, Con.ID) delivery callback was already wired by FC-P1.2**
  (`f->upper->message`/`datagram`, dispatched from `pe_fsm_rx`) — FC-P1.3's
  R1 exercises it end-to-end (send via `pe_vc_send_msg` on one node's
  circuit-shaped fixture, receive via a codec-built peer frame into
  `pe_fsm_rx`) rather than re-implementing it.
- Files touched beyond the plan row's `vms_pe.h`/`vms_pe_fsm.c`:
  `vms_cluster_codec_vc.{h,c}` (the fixup helper — see above; a
  frame-vs-body assembly primitive belongs in the codec TU that already
  owns the sequenced-message envelope, not as a raw offset in the FSM,
  which would break `vms_pe_fsm.c`'s own "not one byte offset in this file"
  invariant); `vms_pe.h` doc comments only (no signature change).

### E8. op-06 MEMBERSHIP record layout is un-isolated → blocks CSID hand-off (raised by FC-P3.12 → LAB)
A coordinated ADD completes the barrier but **never tells the joiner the CSID it
was assigned**, because the op-0x06 MEMBERSHIP burst's `{SCSSYSTEMID, incarnation,
CSID}` record has no isolated byte offset (spec §4(j) RE gaps). FC-P3.12 does NOT
originate op-06 (originating an ungrounded record would be fabrication). Consequence:
- **Blocks FC-P3.3's CSID-learning** (the join learns its CSID by matching its own
  SCSSYSTEMID in membership records — needs the op-06 layout).
- **Blocks FC-P3.10's R2** ("forms and adds a 4th").
- **Needs a LAB capture** of the op-06 MEMBERSHIP record to isolate the layout —
  this is exactly the oracle/spec-capture the lab lane owns. Until then, FC-P3.3
  must advertise/log honestly and NOT invent the record. Also un-isolated (P3.12
  divergences 2–4, all emit explicit zeros, none faked): op-0x05 rebuild burst +
  originating cat-02 op-0d (FC-P5.5 owns the push), Phase-1 proposal cells
  (proposed quorum/CEVOTES/qdisk/foundation/founder/rebuild-type).

### E6. Process rundown must post the proxy release to the master (raised by FC-P4.4 → owned by FC-P4.6)
`lock_teardown_locked` (process rundown) tears a proxy LKB down LOCALLY without
posting the release to the master. `$DEQ` posts correctly (outside all locks),
but rundown's three call sites all hold `proc->lock_list_lock` (a spinlock on
Linux), and `post` is the cluster requester's implementation — calling it there
pushes an arbitrary implementation into atomic context. The outbound path's
context rules belong to the **FC-P4.6** requester FSM. Gap is documented in-line
at `lock_teardown_locked`. **FC-P4.6 MUST close it** (post the master release from
a context that may block), and its done-condition should assert a rundown of a
proxy-held lock reaches the master.

### E7. Repo-wide gates to clear before the origin/main PR (raised by FC-P4.4)
- `identity_ssot_gate` (tests/integration/test_identity_ssot.sh): FIXED — the
  FC-P3.1 `codec_cm.h` version-field comment was reworded to drop the
  double-quoted `"V7.3"` literal (a code line's trailing comment isn't covered by
  the gate's comment-line exclusion).
- `kif_caller_census` ("compile_commands.json contains backslash escapes"): the
  known build-dir backslash trap — environmental, not a source red. Resolve at the
  final-integration gate pass (ensure the census does not scan a build tree's
  compile_commands.json), NOT by editing source.

### E5. FC-P0.9 must not re-add the codec objects (raised by FC-P0.8)
FC-P0.7's CMake comment deferred codec module-wiring to P0.9, but P0.8 already
did it (its FSM couldn't link otherwise). P0.9 must NOT re-add
`vms_cluster_codec.o`/`_hello.o` to any build list.
