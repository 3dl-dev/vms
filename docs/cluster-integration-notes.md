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

### E5. FC-P0.9 must not re-add the codec objects (raised by FC-P0.8)
FC-P0.7's CMake comment deferred codec module-wiring to P0.9, but P0.8 already
did it (its FSM couldn't link otherwise). P0.9 must NOT re-add
`vms_cluster_codec.o`/`_hello.o` to any build list.
