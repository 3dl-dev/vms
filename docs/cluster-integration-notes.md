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
- **FC-P2.2's finding — SCS→port is TWO calls, because the wire is two
  shapes, and that is not a second design.** The body-level seam above is the
  APPLICATION-MESSAGE path: the fixed 190-content class, split port 0–55 / SCS
  56–71 / SYSAP 72–203, and `pe_vc_send_msg` requires exactly
  `PE_SEND_BODY_LEN` (148). SCS's own **connect verbs** (ops 0–9) occupy the
  SHORT SCA classes 58/62/66/110, which no lower layer can pre-build, so SCS
  builds the whole frame through `vms_scs_ctrl_build()` (getting its real
  addressing from `pe_vc_addr`, never inventing a MAC) and hands it to
  FC-P1.2's frame-level `pe_vc_send_frame`, which stamps the sequence. That is
  exactly what `pe_vc_send_frame`'s own doc-comment describes it for. So
  `struct scs_fsm_ops` has `send_ctrl` (→ `pe_vc_send_frame`), `send_msg`
  (→ `pe_vc_send_msg`) and `addr` (→ `pe_vc_addr`), and FC-P2.4's glue is three
  one-line bindings. FC-P2.2 also added the 16-byte body-level SCS header
  builder/parser this needs (`vms_scs_hdr_build`/`_parse`/`_parse_frame`,
  `vms_scs_msg_body`/`_body_build`) to the FC-P2.1 codec TU, so `vms_scs_fsm.c`
  contains no wire offset at all.
- **FC-P2.2's honesty note on abs 36–55 of a CONTROL frame.** `vms_scs_ctrl_build`
  writes the whole frame, but abs 32–55 is the PORT's span in the table above.
  SCS therefore passes explicit zeros for the incarnation echo (36),
  NISCS_LAN_OVRHD (38) and the two observed tail constants (52/54), exactly as
  `pe_vc_send_msg` already leaves 36–55 zero for the 190-content class, and the
  port stamps 32/34/44 at transmit. The abs-72 marker word goes out zero for the
  same reason: §4(h)(1a) grounds semantics for op 6's `marker[2:4]` alone and no
  capture isolates its encoding. **When FC-P1.1's generic 36–55 builder lands,
  both paths should use it**; until then these are documented zeros, not a
  template.
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

### E18. The port cannot ROUTE the 58- and 94-content SCS classes upward (raised by FC-P2.2)
**Blocks the R4 half of FC-P2.4, not FC-P2.2's R1.** `vms_pe_fsm.c`'s
`vc_deliver()` only hands a frame to SCS when the codec grants it
`VMS_FCAP_CONID` — and FC-P0.6's frozen classify table grants that to
`VMS_FCLS_SCS_CONN_CTRL` for content lengths **{110, 66, 62} only** (plus
`VMS_FCLS_SCS_MSG` for 190). So on a real wire:
- a **58-content** short (ops 5 REJECT_RSP, 7 DISCONNECT_RSP, 8 CREDIT_REQ,
  9 CREDIT_RSP) classifies as `VMS_FCLS_SCS_SEQ`, which carries **no** CONID
  capability, and is acked-but-unrouted (`vc_rx_undelivered`);
- a **94-content** directory lookup (op 10) is likewise unroutable — and
  widening the CONN_CTRL rule alone does NOT fix it, because that rule's
  `M_FIELD_NE` guard deliberately excludes op 10.

Consequence: OVMX would send its own `8 → 9 → 6 → 7` teardown and its directory
lookups correctly and never see the peer's answers. FC-P2.2 is unaffected at R1
(`scs_fsm_rx_message()` classifies and parses the frame itself, and its tests
drive it directly), and **sending** all five classes already works
(`pe_vc_send_frame` classifies for the sequence stamp only, and
`VMS_FCLS_SCS_SEQ` does carry `VMS_FCAP_SEQ`).
- **Grounding for the fix:** §4(h)(1b) states the envelope — including the
  Con.ID pair at payload `[50:58]` — is uniform across "the short classes here,
  the 94-content MSCP commands, and the 190-content class". FC-P0.6's narrower
  rule is its own harvest note ("§4(h)(1a) grounds the Con.ID pair … over the
  110/66/62 content classes"), superseded by (1b).
- **Disposition:** owned by **FC-P2.4** (or a small FC-P2.1 follow-up), because
  it edits the FROZEN shared `vms_cluster_codec.c` rule table and wants its own
  codec test. Two edits: add 58 to `VMS_FCLS_SCS_CONN_CTRL`'s length list, and
  give op-10 a CONID-capable class at 94 content (either widen
  `VMS_FCLS_SCS_MSG` to {190, 94} or add a directory class). FC-P2.2 did NOT
  make this change: improvising a classification widening inside an FSM item
  would be exactly the kind of unreviewed frozen-table edit E5 warns about.

### E18 — RESOLVED by FC-P2.1b. The 58/94 classify widen landed (dedicated `VMS_FCLS_SCS_APPLMSG94` class + 58 added to CONN_CTRL; `mscp_seq_ok` widened). FC-P2.3's escalation-1 restating the gap was against a pre-P2.1b base and is moot on the integrated branch. FC-P2.4's R4 receive routing is unblocked.

### E22. Directory credit grant [48:50]=3 vs §4(d)'s "SCS$DIR_LOOKUP 1" (raised by FC-P2.3 → LAB)
§4(h)(2a)'s frame table reads `[48:50]=3` byte-exact on the `SCS$DIRECTORY`
CONNECT_REQ, but §4(d)'s per-SYSAP list says "SCS$DIRECTORY 3, SCS$DIR_LOOKUP 1"
— a contradiction under natural labelling. FC-P2.3 took byte-exact **3** for both
halves, labelled the acceptor's value OVMX's choice. LOAD-BEARING: at 3 the round
completes with each answer piggybacking the freed buffer + ZERO type-8 frames
(§4(h)(1g)); at 1, the partial "dangerously low" threshold (E21) fires an op-8 per
message. **Lab ask: isolate `[48:50]` on the `SCS$DIRECTORY` ACCEPT_REQ.**

### E23. Possible off-by-one in P2.2's 0x5b→0x4b phase rule on the directory connection (raised by FC-P2.3 → LAB/P2.2 follow-up)
P2.2 flips `data_phase` on the first app message a CDT transmits. The clean 2-node
dir connection shows the joiner `5b,5b,4b` across three lookups (flip after the
2nd) while the member is `5b,4b,4b` (after the 1st). FC-P2.3 did NOT touch P2.2's
grounded rule (its tests assert no msgtype on lookups). Needs the capture to
settle; may be a small FC-P2.2 correction.

### E24. Affirmative `VMS$VAXcluster` directory descriptor is RE gap §4(h)(2)(c) (raised by FC-P2.3 → FC-P3.x)
OVMX emits the registered NAME (not a real 16-byte descriptor) for a
`VMS$VAXcluster` HIT unless CNXMAN declares its own via
`scs_fsm_sysap_set_dir_data()`. Whether a real established member accepts a
name-echoed hit is UNKNOWN (lab). **FC-P3.x (CNXMAN glue) must supply the real
descriptor once grounded** — the mechanism (`set_dir_data`) is in place.

### E25. Target-name collision `test_scs_dir` with retired strawman `tests/vmsscs/` (raised by FC-P2.3 → FC-P3.9 cleanup)
`tests/vmsscs/` (the strawman) owns CMake target `test_scs_dir`; FC-P2.3's is
`test_scs_directory`. Reclaim the name when **FC-P3.9** retires the strawman tests.

### E19. `scs_sysap_ops.closed` IS design §3.2.5's `disconnected()` (raised by FC-P2.2)
The design and the plan row say a VC break "calls each SYSAP's
`disconnected()`"; the FROZEN FC-P0.1 interface spells that callback
`closed(ctx, local_conid, reason)`. FC-P2.2 kept the frozen spelling rather
than adding a second callback for the same event — a SYSAP is told ONCE that a
connection is gone, with the reason that took it (`SCS_CLOSE_PATHLOST` →
`SS$_PATHLOST` in the glue). `vms_scs.h` now says so at the callback. **Name
difference only, no behavioural divergence.** FC-P2.2 did ADD one optional,
NULL-safe callback the design implies but the frozen struct lacked:
`send_failed(ctx, conid, reason)`, so a message that was in *Credit Wait* when
the path died is reported rather than evaporating (§3.2.5: "pending sends fail
`SS$_PATHLOST`" — Credit Wait is what a pending send IS, *VAXcluster
Principles* ch. 2).

### E20. FC-P2.3 must GROW FC-P2.2's SDIR seed, not add a second registry (raised by FC-P2.2)
The plan puts the SYSAP registry in FC-P2.3 but names `vms_scs_fsm.c` as where
it lives — and FC-P2.2 could not test a single inbound connect without one. So
FC-P2.2 landed the minimal ch. 2 shape: an **SDIR table** (`struct scs_sdir`,
`SCS_MAX_SYSAPS`) plus a **listening CDT** per registered name, with
`scs_fsm_listen()/_unlisten()`. That is what makes the frozen ladder's
`VMS_SCS_CDT_LISTEN` and `_CONNECT_RCVD` states real: ch. 2 puts the listening
CDT in CONNECT RECEIVED while the SYSAP decides, returns it to LISTEN when the
request is answered, and allocates the connection's OWN CDT only on ACCEPT.
**FC-P2.3 adds on top:** the `vms_scs.h` service wrappers
(`scs_sysap_listen/_unlisten/_connect/_accept/_reject/_send_msg/
_return_credit` → their `scs_fsm_*` twins) and the `SCS$DIRECTORY` SYSAP. It
must NOT introduce a second name table.

### E21. SCSFLOWCUSH is not in `struct vms_cluster_params` (raised by FC-P2.2)
p. 2-44's "dangerously low" test is `local Receive Credit < SCSFLOWCUSH +
remote Minimum Send Credits`. FC-P2.2 holds the cushion in `scs_fsm_cfg`
(default 1, the PUBLISHED VAX/VMS V7.3 SYSGEN default — at which a real VAX
emitted **zero** type-8 frames in 440 367, §4(h)(1g), which the default
reproduces). It is NOT in the SYSGEN struct that crosses
`VMS_IOCTL_SYSGEN_LOAD`, because that is FC-P0.10's ABI and an FSM item should
not churn it. **FC-P0.10 or FC-P2.4 should add `scsflowcush` to
`vms_cluster_params`** and have the glue pass it through `scs_fsm_set_cfg()`.
The second term (the peer's Minimum Send Credits) has **no grounded wire
offset** anywhere in the spec, so it is never guessed: the CDT carries
`peer_min_send_credits` with a `_valid` flag that nothing yet sets, and every
firing of the partial threshold is counted in
`scs_fsm.credit_msg_partial_threshold`. A lab capture isolating that field
would close it.

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

### E14. SYSGEN_STRVAL_LEN=8 truncates long DISK_QUORUM device names (raised by FC-P0.10)
`tools/vms_sysgen.c`'s `SYSGEN_STRVAL_LEN` is 8 bytes (sized for SCSNODE). A real
quorum-disk device name longer than 8 chars (e.g. `$102$DGA1023`) truncates.
Out of FC-P0.10's scope; documented in-code. **Widen `SYSGEN_STRVAL_LEN` (and the
wire struct) before the quorum-disk path (FC-P0.13/P8) actually uses DISK_QUORUM
by device name** — or it will address the wrong disk. Not blocking today (no
quorum-disk consumer yet).

### E15. Decoded `vms291` serving capture for the block-transfer oracle (raised by FC-P6.1)
FC-P6.1's plan row asks for "R1 byte-exact on the vms291 mount capture frames".
The three pcaps (`vms291-mount-A`, `vms291-control-B`, `vms291-boot-C`, lab-2
`vaxlab-9`, 2026-08-06) are **host-only, never in git**: absent from
`docs/clean-room/reference-captures.sha256` and absent from this machine. What
IS in tree is the DECODE, in `docs/design-mscp-direction.md` ("Phase D part 1's
lab capture — SCA block data transfer, DECODED"), and `tests/cluster/host/
test_pe_block.c` asserts against exactly that: the 28-byte field table
byte-for-byte, and all **five** recorded READ-END SCA content lengths
(118/194/448/630/1142), each of which the builder reproduces exactly as
`(58+32) + 28 + tail` for tail ∈ {0, 76, 330, 512, 1024}. That arithmetic
closing on all five with no residual is the strongest available check without
the packets.

**Lab lane, three asks, in priority order:**
1. **Decode `vms291-mount-A` to a `.spec` fixture** so a frame-for-frame byte
   comparison replaces the length/field-table comparison.
2. **The `+4` / `+6` words.** Still ungrounded ("Still ungrounded, do not build
   on"). OVMX emits an explicit **zero** for both unless it has OBSERVED a pair
   on that circuit, and counts the zeros (`pe_fsm.blk_obs_absent`); it bakes in
   neither the captured 9 nor the 13. Two questions the lab can settle from the
   existing pcaps without new hardware: does the value at +4 differ between the
   two DIRECTIONS of one connection (which would make it a per-endpoint id
   rather than a per-connection one), and does +6 restart or continue across a
   connection teardown?
3. **Does a real port RETRANSMIT a block frame, and by what mechanism?**
   FC-P1.2 excluded block frames from the unacked ring (a 1498-byte frame will
   not fit a ring sized for the 204-byte message class) and FC-P6.1 honours
   that, so OVMX does **not** retransmit one; recovery is MSCP's own host
   timeout (`P.HTMO`). Every such frame is counted in `pe_fsm.blk_tx_unringed`.
   If the capture shows a real server re-sending block frames — or a distinct
   ack for them — that is a fidelity gap to reopen with a sized design, not a
   silent one.
4. **Does credit/flow control interact with block transfers?** Also on the
   "do not build on" list. FC-P6.1 does **not** debit Send Credit for a block
   frame: p. 2-43/2-44 gives the account to the sequenced *message* service and
   the book bounds a block transfer by the NAMED BUFFER the far SYSAP sized
   instead. If vms291 shows a real port's credit ledger moving with block
   frames, that is a one-line change with an oracle behind it.

**One judgement FC-P6.1 made and labelled, for the record.** Block frames
consume the circuit's REAL `send_seq` (and its abs-44 mirror), because spec
§4(k)'s correction places the class in the `0x4b`/`0x13` sequenced-application
family and §4(h)(4) grounds that every frame of that family stamps one
(17,758/17,758). The alternative — borrowing the datagram service's
"`send_seq = 0`, no ordering claimed" shape — would be a wire claim nothing
measured. The census in §4(h)(4a) cannot discriminate the two (it filters on
`send_seq != 0`), so a decoded vms291 settles it too.

### E16. kif_caller_census WILL red main CI when this branch lands (confirmed FC-P6.1; diagnosed by integrator)
`test_kif_caller_census.sh` fails "compile_commands.json contains backslash
escapes" — the blanket `grep -q '\\' "$CCJ"` pre-check at ~line 1030. Root: the
cluster host tests' `target_compile_definitions(... OVMX_FIXTURE_DIR="${dir}")`
(tests/cluster/host/CMakeLists.txt) emit `-DOVMX_FIXTURE_DIR=\"/path\"` whose
JSON-escaped quotes are legitimate backslashes.
**PRECISE FIX GUIDANCE (do it RIGHT — this is an INV-6 gate, do NOT weaken it):**
- The census ALREADY scope-excludes test TUs (its final awk keeps only
  `^(src|tools)/`), and product TUs (src/tools) carry NO such defines → no
  backslashes. The only problem is the GLOBAL pre-check running before that
  filter. The census's own comment sanctions: "teach the reader to unescape; do
  NOT relax the census."
- **Preferred fix:** either (a) teach the awk line-reader to unescape `\"`/`\\`
  in the `command` field (then the pre-check can go) — verify with the census's
  own negctl/self-test that it STILL catches a real mis-split; OR (b) apply the
  backslash guard only AFTER filtering to `src/tools` lines (product-only), so a
  real product escape still refuses but a test-TU escape is ignored — same
  protection, correctly scoped. Do NOT just delete the check.
- **Alternative (broader, avoid unless a/b fail):** move the fixture paths to a
  `configure_file` generated header so no `-D"..."` define exists — but that
  touches CMakeLists + ~15 test .c files.
- **DISPATCH after P2.3 lands (CMakeLists quiet). Pre-PR blocker: must be green
  before `feat/cluster-executive` merges to main.** Supersedes E7.

### E17. ⚠ COMPAT-REGISTER OVERCLAIM: cluster-dlm claims distributed DLM COMPLETE (escalated to operator — INV-0/authenticity)
`docs/compat/facilities/cluster-dlm.yaml` (→ `docs/compatibility-surface.md`, and
MIRRORED PUBLICLY on the site) states the distributed DLM is **COMPLETE** — "every
rung H0→H11 proven on real /dev/vms," dynamic REMASTERING, DIRECTORY OWNERSHIP,
LVB, distributed DEADLOCK — all `status: verified`, `authenticity: real`, via the
`run_dlm_harness_h*.sh` two-node OVMX↔OVMX QEMU rail. **This contradicts the reset
premise + the design audit:** the distributed coupling those rungs exercised was
driven by the harness/retired-scsd path, NOT a real executive-resident PEDRIVER/
SCS/CNXMAN stack; the executive-resident directory/rebuild FSMs (FC-P5.3/5.4/5.5)
are NOT YET BUILT; and per [[h2-green-not-real-vax-join-proven]] a two-node OVMX
harness ≠ a real cluster join. The engine's cross-node GRANT/BLKAST/LVB logic IS
real (FC-P4.4 R4 dlm_xnode 42/0) — so the fix is SCOPING, not erasure: "engine
cross-node logic proven on the 2-node OVMX /dev/vms harness; executive-resident
distributed DLM (real interconnect-coupled + real-VAX interop) IN PROGRESS." **NOT
corrected unilaterally: it's the capability SSOT + public content (INV-0) + an
authenticity-posture call = operator-reserved. TEED UP to operator.**

### E26. SS$_PATHLOST/INCONSTATE/NOSUCHNODE have no value in the tree (raised by FC-P2.4 → LAB, one-line fix)
Those status codes aren't defined anywhere. FC-P2.4 used `SS$_DEVOFFLINE` (2692,
real in ssdef.h) as a documented placeholder in `pe_send_status()` +
`scs_glue_status()`, added `SS__DEVOFFLINE` (both substrates) + `SS__ABORT`
(NetBSD twin). **Lab ask: extract `$SSDEF` on the VAX** (same published-macro
route as the `$SCSDEF` oracle) → then a one-line correction. Never invented a
number (Rule 8).

### E27. SDA connection state-name mismatch (raised by FC-P2.4 → FC-P2.6 lab)
Spec §4(O.25): real SDA prints `0002 open`, `0007 con_sent`, `0001 con_pend`;
OVMX's frozen `scs_cdt_state_names` are `open`/`connect sent`/`connect rcvd` and
OPEN=6 not 2. Only `open` matches by string. Frozen ABI (re-deriving VMS's
numbering from captures is Rule-8 territory) — so an FC-P2.6 lab SDA comparison is
a string match for `open` ONLY. Not changed; flagged for the P2.6 oracle.

### E28. SCSCONNCNT/SCSFLOWCUSH not wired to their consumers (raised by FC-P2.4; extends E21)
FC-P0.10 LOADS the SYSGEN params but `SCSCONNCNT` (CDL sizing) and `SCSFLOWCUSH`
(credit cushion, E21) aren't read by the SCS layer yet: `SCS_CDL_ENTRIES=128` is a
labelled OVMX bound with an honest `SCS_ERR_NOCDT` refusal, not p. 2-29's
`SCSCONNCNT + 200`. **Small FC-P0.10 follow-up: wire these two loaded params to
the SCS consumer.** Not blocking.

### E29. FC-P3.8 owns the first SYSAP that acts on a `closed`/`vc_down` reason (raised by FC-P2.4)
`scs_sysap_ops.closed`'s `reason` stays `enum scs_close_reason` in kernel-core
(design §3.2.2 keeps SS$_ out of kernel-core cluster headers); the glue maps to
SS$_ only where a status is RENDERED. **FC-P3.8** (CNXMAN glue) is the first SYSAP
that acts on a reason — it should confirm or re-word the `vms_scs.h` header note.
Also: FC-P2.4's R4 two-node lookup + FC-P2.6's R5 are structurally gated on
**FC-P3.3** (nothing calls `scs_dir_lookup` until the join drives it — the harness
reports PENDING, never a false pass).

### E30. ⭐ THE SINGLE BLOCKING GAP TO CN=3 MEMBER: op-06 SCSSYSTEMID layout (raised by FC-P3.3 → LAB, now cheap)
The join FSM reaches the barrier and completes the dialogue but **cannot reach
MEMBER** because it will not fabricate its CSID (E8). FC-P3.3 landed the
INSTRUMENT that makes this a one-capture fix: `vms_cm_membership_find_sysid()`
searches a real op-0x06 body for OVMX's OWN SCSSYSTEMID (a value it owns) and
reports the **byte offset + width** (`join.sysid_seen_at`/`_width`), reading
nothing off it. **Lab ask: run a booted join OR decode a real op-06 body; the
reported offset+width + one record's neighbourhood pins the {SCSSYSTEMID,
incarnation, CSID} layout.** Then `cnxman_join_csid_learned()`'s already-R1-tested
cell fires and the node reaches MEMBER. This is THE lever between the built stack
and the milestone. (Supersedes/sharpens E8.)

### E31. ⚠ OPERATOR DECISION: OVMX's `VMS$VAXcluster` connect-data (CM version identity) (raised by FC-P3.3)
The 16-byte `VMS$VAXcluster` connect data is the CM **version handshake with a
REJECT right attached** (p. 2-25). §4(N): OVMX "cannot yet generate connect data
for a role it has not observed, and it must not claim to." The strawman shipped a
replayed capture constant (`01 1b 01 03 …`). FC-P3.3 made it
`cnxman_join_cfg.conndata` — glue-supplied or an explicit COUNTED zero — and baked
nothing. **A real member may REJECT a zero, blocking the join.** This is an
honest-identity ruling (reserved): **the operator must decide what OVMX declares
as its cluster-protocol version identity.** Teed up. Absent a ruling, the join
sends a counted zero and logs it (honest but may be rejected by a real VAX).

### E32. Field-pinning items sharpened by FC-P3.3 (→ FC-P3.2 / lab)
- **LOCKDIRWT (FC-P3.2):** =0 and "field not written" are the SAME bytes today —
  coincidence, not placement. `lockdirwt_unpinned` counts every PARAMS; a NONZERO
  configured LOCKDIRWT sets `lockdirwt_unrepresentable` + logs loudly (understating
  a directory weight would misroute directory duty). FC-P3.2 must pin the offset.
- **D7 coordinator selection:** protocol/ECO level + the "connected==advertised"
  precondition have no isolated offsets; FC-P3.3 implemented the residual rule
  (CSB nearest CLUB queue tail, p.7-38) and counted the rest
  (`target_level_unpinned`, `member_count_ungated`) — did NOT gate the join on a
  count it can't read (deadlock-over-omission avoided). FC-P3.2 pins these.
- **E24 dir descriptor:** mechanism wired (`dir_descriptor` → `set_dir_data`),
  `dir_descriptor_omitted` counts it; still needs the lab byte.

### E33. FC-P3.3 minor couplings (→ noted owners)
- **R2 partial (E12):** the vax3 established-join replay uses in-tree fixtures; two
  elements (op-0a GO, MSCP ENDs) are codec-built (no specimen) and say so. Swap in
  a decoded vax3 fixture = one line in `scenarios/cnxman_join.c`.
- **MSCP body-level:** the join sends MSCP body-level (all-zero link, `frame[72:108]`)
  — E1-correct + stronger than P3.4's frame-level link; if FC-P6.2 grows body-level
  MSCP builders this is a 2-line simplification.
- **Directory teardown:** FC-P3.3 uses `vms_scs_dir`'s p.2-51 close-when-idle; if the
  lab shows §4(o)'s later teardown is load-bearing, it's an FC-P2.3 change.
- **`CNXMAN_EV_RX_CLOSE` dual-purpose:** barrier maps it to cat-01 op-04 ABORT, join
  to cat-06 op-00 close — tables never see each other's frames; flagged not renamed
  (frozen enum).

### E16 — RESOLVED (FC-E16, tip 5d7dd9c9). The backslash false-failure is fixed: the guard is now scoped to product TUs (`^(src|tools)/`) after filtering, so test-only `-DOVMX_FIXTURE_DIR="…"` escaped quotes no longer trip it; teeth preserved (verified: a product-TU backslash → refused; a kif-only-in-tests caller → FAILed). The census now fails for a DIFFERENT, real reason → E34.

### E34. ⚠ PRE-PR BLOCKER: cluster DIAG ioctls have no product caller → census red (unmasked by E16)
`kif_caller_census` (repo-wide gate, exit 1) now fails: **`VMS_IOCTL_CLUSTER_DIAG_PORT` — no `vms_kif.c` wrapper ever issues it.** Same will apply to `CLUSTER_DIAG_CONN` (FC-P2.4) and `CLUSTER_DIAG_CSB` (FC-P3.8) — all diagnostic ioctls reachable ONLY through a test tool = the facade shape the census correctly catches. NOT in the cluster integration gate loop (cluster_host/includes/check_guest_payload all pass) — it blocks the FINAL PR to main only.
- **Proper fix = FC-P3.9's SHOW CLUSTER** becomes the real product caller: P3.9 adds `SHOW CLUSTER`/`$GETSYI` reading the executive → give each cluster DIAG ioctl a `vms_kif.c` wrapper that SHOW CLUSTER (or the cluster-diag CLI) issues → census green with a REAL product path, no OVMX-UNWIRED throwaway. **FC-P3.9 MUST wire kif wrappers for DIAG_PORT/CONN/CSB and their SHOW-CLUSTER caller** (mind the "new kif symbol → N places / shr.vec" trap). Do NOT let feat/cluster-executive PR to main while census is red.
- Interim OVMX-UNWIRED declarations are the fallback ONLY if P3.9 slips — but they'd be throwaway, so prefer the real caller.

### E35. CLUSTER_MEMBER_GET repoint deferred — would regress a live scsd test (raised by FC-P3.8 → FC-P3.9)
P3.8's plan row asked to repoint `CLUSTER_MEMBER_GET` at the CSB table; P3.8 did
NOT, deliberately: `tests/qemu/test_syssvc_cluster_member.c` +
`tests/integration/test_show_cluster_membership.sh` drive the legacy scsd-populated
`vms_cluster_member_set/get` table (incl. a `$GETSYI` cutover assertion). Since
CSBs never learn a CSID today (E30), repointing GET would make it permanently
report 0 members — a real regression. **FC-P3.9 does the repoint AND retires the
scsd-based test together** (P3.9 retires the strawman anyway) — or explicitly
descopes the repoint until E30's op-06 layout is lab-pinned. Left untouched.

### E36. Peer-discovery → CSB-allocation is unwired — join hits NO_TARGET even with a real peer (raised by FC-P3.8 → FC-P3.9 or new item)
Nothing yet turns "the port saw a peer's HELLO/START (P0.8/P0.9 channel up)" into
"allocate a CSB for that peer + point the join at it." Until that exists, every
join attempt returns `NO_TARGET` even with a real peer present — an R4/lab-blocking
gap between the port (P0.9) and CNXMAN (P3.8). **FC-P3.9's "CLUSTER_START join
semantics" should wire peer-appears→CSB-alloc→join-target; if that's beyond P3.9's
file list, it's a new small item on the tier-2-boot path.** This + E30 (CSID) +
E31 (conndata) are the three things between the built stack and a peer actually
joining.

### E37. FC-P3.9 scope note: do NOT resolve E17 (compat-dlm overclaim) while updating compat rows
P3.9's row includes "compat register rows updated." It should ADD honest rows for
the NEW executive cluster facilities (PEDRIVER/SCS/CNXMAN/join — status per what's
actually built + verified: R1/R3 real, R4/R5 lab-deferred, NOT "complete"). But it
must **NOT** unilaterally rewrite the **E17** `cluster-dlm.yaml` distributed-DLM
overclaim — that's operator-gated (INV-0). Flag E17 as still-open; leave the
resolution to the operator's ruling. P3.9 also owns making `kif_caller_census`
green (E34) via the SHOW CLUSTER product caller + kif wrappers for
DIAG_PORT/CONN/CSB.

### E34 — RESOLVED by FC-P3.9. kif census exits 0 (65/65 opcodes wrapped); SHOW CLUSTER + $GETSYI are the real product callers for DIAG_PORT/CONN/CSB + a new CLUSTER_GETSYI (0x6c). `$SETCLUEVT` wrapper honestly `OVMX-UNWIRED (vms-733)` — no `$SETCLUEVT` service in src/libvms yet (that service = a separate outcome).
### E36 — RESOLVED by FC-P3.9. Peer discovery WIRED: `cnxman_discover_peers()` sweeps the SCS SB table on the reconnect beat, allocates a CSB (NEW) per system with an open circuit; join gated on `cnxman_join_target_present()`. sysid traces to a real received frame (`vc_notify_up`). No NO_TARGET-with-a-real-peer anymore.

### E38. FC-P3.9 aftermath — deferred module params, compat downgrade, E17 now dangling (raised by FC-P3.9)
- **DEFERRED (divergence from plan row, flagged):** `vms_local_csid`/`dlm_member_csids` module params NOT removed — they are the legacy `vms_lock.c` directory's only CSID source, which **FC-P4.3 owns replacing** (`exec_jhash`→`dir_resolve`). Removing now amputates the DLM directory with no replacement. The executive stack never reads them; only tests do. Safe to defer; **FC-P4.3 removes them with its replacement.**
- **COMPAT DOWNGRADE (honest, forced):** `mscp-serve$disk-read-write` → **status=absent** because its implementation WAS the deleted scsd daemon. `connection-manager$real-vax-join` → **absent** (names E30/E31 as the gap). These are INV-6-honest corrections of strawman-backed claims, not regressions of real capability.
- **E17 now has 2 DANGLING-EVIDENCE warnings** in `cluster-dlm.yaml` (`src/vmsscs/scsd.c`, `scs_dlm.c` — deleted by P3.9). P3.9 left them deliberately (repairing the pointer = editing the row the operator must adjudicate). **E17 is now MORE visible + MORE urgent for the operator:** the cluster-dlm "distributed DLM COMPLETE" claim's own cited evidence (the scsd daemon) is GONE, which underscores that the claim was strawman-backed. Operator ruling still pending.
- **rd items FC-P3.9 filed** (retirement fallout, tracked): vms-ec8 (24 lab capture scripts → fold into FC-P3.11), vms-1da5 (cross-node DLM CI gone until FC-P4.8), vms-1b6 (~25 spec-conformance/anti-drift gates → restore quarantine gates first), vms-733 ($SETCLUEVT service unbuilt).

### E39. FC-P6.3 extended the SCS send path to length-generic MTYPE-10 (grounded §4(h)(1b))
SCS grounded send paths only for 190- and 94-content; 3 of the 5 measured MSCP end
classes (86/102/110) had NO send path. FC-P6.3 added `pe_vc_send_msg_var` →
`msg_transmit_var` (one shared assembly, not a second design), grounded on the
SAME §4(h)(1b) uniform-envelope rule Fable ruled for E18. Also implemented FC-P6.1's
declared-but-empty `pe_buf_register/_release/pe_send_block` + `pe_send_block_read_end`.
Consistent with the ruled body-level contract — noted for the record, not a new
ruling. Compat: `mscp-serve$disk-read-write` RESTORED to implemented/real (E38
downgrade reversed — see mscp-serve.yaml); did NOT touch cluster-dlm.yaml (E17).
- **FC-P6.3 lab-asks (→ FC-P6.4/lab):** (1) unit 0 unreachable by FC-P3.4's MD.NXU
  walk (client seeds unit 1, §6.12 is `>=`, DKA0: is unit 0) — both sides assert
  the consequence so it can't drift; needs "what does a real VMS class driver do";
  (2) WRITE block-transfer INITIATION direction ambiguous (READ fully grounded);
  (3) SCC-END `0xa004`/`0x0547` are undecoded constants — server reports real flags,
  doesn't replay; (4) `$MOUNT` has no /NOWRITE surface yet so `read_only`=0 (the
  host-requested MD.SWP write-protect path IS real).
- **FC-P6.3 caught + fixed 2 real defects:** shared staging buffer → concurrent-WRITE
  data corruption (now per-HRB disjoint slots); partial-block commit reporting false
  success (now ST.HST, nothing written). Third and fourth silent-data bugs the
  faithful build caught that a bullseye-chase ships.

### E40. ⚠ ALLOCATION CLASS has no grounded transport — blocks `$n$DUAn` spelling (raised by FC-P7.1 → Fable/lab)
Design P7 wants `SHOW DEVICE` to list `$2$DUA0:`. A class driver can only spell that
if it knows the SERVING node's ALLOCLASS — but NO executive structure carries it:
`vms_csb_view` (SCSNODE/VOTES/LOCKDIRWT/version/incarnation), `vms_cm_params`
(VOTES + node-param block), MSCP (unit number + identifier) — none has an allocation
class. FC-P7.1 refused to guess `$2$` (two members serving unit 0 under different
alloc classes would collide → data-loss) and emits node-qualified `<SCSNODE>$DUAn:`,
counting `alloclass_absent`; `mscp_cl_unit_name()` already takes `(alloclass,
valid)` so `$n$` is a one-line change. **Fable/lab: where does a real VMS class
driver learn a serving node's ALLOCLASS from?** (which frame/field carries it).

### E41. WRITE block-transfer INITIATION is field-map-forced to server-driven (raised by FC-P7.1 → Fable/lab)
Only the SERVER knows both buffer names (it reads the client's off the command +
mints its own), so the CLIENT cannot initiate a WRITE data frame. Open question:
is the two-byte-identical-header form a **server-sent REQUEST** the host's port
answers with data, and does the port need an automatic responder? FC-P7.1 did NOT
add one (would assert the direction). **WRITE never completes today** — the
deadline reaps it, `writes_undelivered` counts it. READ (what MOUNT needs) is fully
grounded + end-to-end proven. Fable/lab ruling needed on the WRITE choreography.

### E42. ACP block seam sync-vs-async — the bridge is FC-P7.2's design question (raised by FC-P7.1)
`exec_blockdev_read_block/_write_block` is SYNCHRONOUS by contract; an MSCP transfer
completes ASYNCHRONOUSLY on the same fork context → a blocking wrapper deadlocks by
construction. FC-P7.1 shipped the async service (`vms_mscp_cl_read/_write` +
completion) and did NOT bridge to the sync ACP seam. **FC-P7.2 owns the bridge (the
ACP's waiting discipline).**

### E43. ⚠ POSSIBLE JOIN BUG: `cnxman_mscp_opened()` is a no-op → MSCP-open gate may be dead (raised by FC-P7.1 → verify + P3.x fix)
FC-P7.1 observed `cnxman_mscp_opened()` is a no-op, so `join_h_mscp_opened` /
`CNXMAN_EV_CDT_OPEN` on the MSCP CDT appears UNREACHABLE — CNXMAN's join walk may
never set `j->mscp_open` (only `cnxman_vc_opened`→`cnxman_join_opened` fires). If
real, the join's MSCP-discovery gate never advances. FC-P7.1 preserved existing
behavior. **Integrator verifying; a real gap is a small P3.x fix to wire
cnxman_mscp_opened → the join's MSCP CDT-open event.**

### E44. Durable trap: reading a CSB from a fork-context beat (raised by FC-P7.1, fixed)
`cnxman_get_csb()` takes the fork mutex; a beat running ON the fork context +
`exec_mutex_t` non-recursive = deadlock. FC-P7.1's MSCP beat hit this near-miss and
now uses `cnxman_club_find_sysid()` (the fork-context accessor). **Any future glue
reading a CSB from a beat MUST use the fork-context accessor, not `cnxman_get_csb`.**

### E40/E41/E42 — RULED by Fable (design §3.2.6); spawned FC-P6.5/6.6/6.7/7.2/7.3
- **E41 (WRITE) RESOLVED, no capture:** WRITE is a **server-initiated REQUEST DATA** the client's PORT auto-answers (Davis pp.2-32..2-41: SEND DATA=READ, REQUEST DATA=WRITE, both initiated by the side knowing both names = the server). Matches vms291's "two byte-identical headers, data presence differs." FC-P6.1 modelled SEND DATA only. **Unblock = FC-P6.5** (port REQUEST DATA responder: lookup source name→transmit with READ's chunking, +4/+6 echoed, +8 counting down, unknown buffer⇒drop+counter; byte-exact vs vms291 WRITE pair). BUILDABLE NOW (dep P6.1/6.3/7.1 all done).
- **E42 (ACP bridge) RESOLVED:** VMS parks a $QIO IRP, fork-level end-message completes it, post-processing wakes the requester. **FC-P7.2 contract:** served `vms_devtab` block ops run in the ACP caller's PROCESS context — `vms_srvdisk_irp{lk,cv,done,status}`, register named buffer, post to fork queue via rxlock, `exec_cv_wait_timeout` on the IRP's own lock (P.CTMO+margin), honest SS$_TIMEOUT/PATHLOST; `srvdisk_done` on the fork thread sets status + broadcasts under irp->lk (leaf under fork mutex). **THE FORK THREAD NEVER WAITS ON AN IRP.** (P7.2 dep P6.5+P5.9.)
- **E40 (ALLOCLASS) — lab-gated, fallback faithful:** alloclass is a CONTROLLER attribute the MSCP server impersonates (learned beside the connection, not per unit). Candidates: SCC-end controller-param area (the unexplained `0x0547`), MSCP$DISK 16-byte connect data §4(N), CM PARAMS block. **FC-P6.7 (lab):** clone VAX1, change ALLOCLASS, reboot, diff the 3. `<SCSNODE>$DUAn:` is EXACTLY VMS's class-0-server rendering → keep it (INV-6). **FC-P7.3** = one-line switch once pinned.

### E45. ⚠ 9th BUG (Fable-caught): FC-P6.3 server does SYNCHRONOUS disk I/O on the cluster fork thread → FC-P6.6
`vms_mscp_srv.c:219/239` call `exec_blockdev_read/write_block` from the fork work
handler — so HELLO cadence + every member's barrier latency STALL behind each
served-disk read. VMS's server issues local I/O asynchronously. **FC-P6.6 (dep P6.3,
BUILDABLE NOW):** move served I/O to a worker kthread posting completions back to
the fork queue; **CI grep gate: the cluster fork thread NEVER calls `exec_blockdev_*`.**
Real latent bug in just-landed P6.3 — the review discipline caught it before the lab would.

### E46. 10th BUG (FC-P6.6-caught + fixed): pre-existing kernel use-after-free in FC-P0.5's Linux exec_kthread binding
`kthread_stop+0x48` NULL-deref (`RAX=0`, usage refcount already zero): a thread body
that returns on its own is self-reaped by Linux (frees `task_struct`) BEFORE the
join. FC-P6.6 PROVED it pre-existing (reproduces on a booted throwaway worktree at
28cee71b, pre-P6.6) and fixed it — the handle holds a task ref
(`get_task_struct`/`put_task_struct`), matching NetBSD's `KTHREAD_MUSTJOIN`;
contract in `exec_kbackend.h §15`. A real kernel crash on boot, latent in P0.5 since
the fork context landed — surfaced only when the P6.6 worker made it reliable. FIXED,
integrated. (Was "outside P6.6's row" but it's all one cluster lane.)

### E47. ⚠ PRE-BOOT-SMOKE: `test_kmod_cluster_{conn,membership,vc}_diag` return nonzero in the QEMU kmod (raised by FC-P6.6)
The CLUSTER_DIAG_CONN/CSB/PORT ioctls pass HOST tests but the QEMU kmod diag tests
return nonzero. FC-P6.6 proved conn/membership pre-existing at 28cee71b (vc_diag never
ran — the E46 oops killed the boot first). **Needs investigation BEFORE the tier-1
wire smoke** (SHOW CLUSTER uses these — if they truly fail in the kmod, SHOW CLUSTER
won't work on a booted node). **CONFIRMED REAL BUG (integrator checked):** the test comment (line 11) says the
diag ioctl "must DISPATCH on a booted node whether or not" the cluster is started —
i.e. dispatch-always, empty rows when not started, NOT an error. The `ioctl()<0`
path fires, so the handler returns an error (likely SS$_NOSUCHDEV when the port/SCS
isn't initialized) instead of SS$_NORMAL + empty. Fix = the CLUSTER_DIAG_PORT/CONN/CSB
handlers return success + a zero-row/empty snapshot when the cluster/port isn't
started (a diagnostic reads state, it doesn't require the subject to exist). Needs
QEMU-boot verification (host tests can't catch it) — lab-defer the boot proof,
fix + reason + cross-compile provable here. **Fix before the tier-1 wire smoke
(SHOW CLUSTER depends on it).** Disjoint from the P6.5/P7.2 chain.

### E47 — RESOLVED (FC-e47, tip e2f278c6). Root cause was NOT the handlers (they correctly return SS$_NOSUCHDEV + zero row + ioctl-success): Linux `vms_dev_ioctl` required a registered VMS process (`vms_proc_find_or_err`→-ESRCH) before ANY ioctl, so raw diag tests failed before reaching the handler. Fix: moved the 3 read-only DIAG ioctls ahead of the registration gate (they `(void)proc`), mirrored NetBSD like GETSYIMEM. SHOW CLUSTER works on a booted node. QEMU-boot verify lab-deferred.

### E48 — RULED by Fable (§3.2.7) → FC-P2.7 (buildable, no capture). YES the Con.ID envelope extends to the MSCP END classes: SCS dispatches on MTYPE=10→Con.ID's CDT, NEVER length (length-classes were a census convenience). Already MEASURED on the 86/90/102/110 END frames (MTYPE@[46:48], handle pair@[50:58]). Fix = new class VMS_FCLS_SCS_APPLMSG keyed on content[44:46]==0x0004 ∧ [46:48]==10 ∧ [42:44]==len−44 at ANY length, grants CONID@[50:58], ordered AFTER conn-control (ctrl_type keeps MTYPE 0-9), SCS_APPLMSG94 kept as no-regression alias. ORIGINAL GAP (for record):
FC-P6.5's R2 (first to push END messages through a real port) measured: `vc_deliver`
gives a SYSAP a frame only when the classifier grounds a Con.ID for its class. Of the
5 measured END lengths — SCA **86 (SCC), 90 (READ), 102 (ONLINE), 110 (GUS)** carry
no Con.ID class (110/102 fall in the conn-control lengths but are excluded by the
`ctrl_type != 10` guard; 86/90 match nothing) — **only 94 (WRITE) is deliverable.**
So a booted MSCP class driver receives almost no end messages. FC-P6.3 gave the SEND
side a length-generic entry (`pe_vc_send_msg_var`); the RECEIVE side never grew the
matching half. FC-P6.5 did NOT widen a codec class rule (§4(d) leaves 64/68 undecoded
for these; asserting it would be a self-made wire claim, Rule 8) — the R2 scenario
carries what the port can + counts `vc_rx_undelivered` on the rest.
- **ESCALATED to Fable:** does §4(h)(1b)'s uniform Con.ID envelope ([50:58]) extend to
  the MSCP END classes 86/90/102/110 (as E18 established it does for the 58/94 SCS
  classes + 94 MSCP command)? If yes → a P2.1b-style receive-classify widen + codec
  test (add a CONID-capable class for those END lengths). If not grounded → lab capture.
  **Blocks FC-P6.5's R4 + any booted MSCP class driver receiving end messages.**

### E49. FC-P4.3 landed dir_resolve (DLM rebuild chain unblocked) — carried residuals
- **body[10:12] dir-hash offset is INFERRED** (FC-P4.2's offline confirmation hasn't run). FC-P4.3 built the consumer so a wrong offset SHOWS UP (`dir_hash_conflicts` + `dir_lookup_misaddressed` counters), not corrupts. **FC-P4.2 should land before R4/R5** (offline confirm body[10:12] = the hash, constant per name).
- **`vms_local_csid` deliberately STAYS** (only `dlm_member_csids` was removed) — re-pointing it at the CLUB's LEARNED local CSID is **FC-P4.8's** glue (so E38's "P4.3 removes both" is half-done: dlm_member_csids gone, vms_local_csid → P4.8).
- **`vms_lock_dlm_learn_dir_hash()` has no production caller yet** → **FC-P4.6/P4.8** wire it (the requester learns the hash off received cat-02 frames).
- **Root-vs-sub-resource:** RSB has no parent link so every resource is treated as root — over-counts lookups, never mis-routes → FC-P4.6/P5.5.
- **Operator note:** a conflicting hash-learn returns `SS$_BADPARAM` (`SS$_DUPLNAM` isn't a real SS__ value; won't invent one, Rule 8). Operator can add a published SSDEF code if a distinct one is wanted.

### E30 — ✅ FIXED (fc-e30-csid-fix @ 086aa8a5): the CN=3 CSID lever is LANDED at R1
The join FSM now reads the coordinator CSID off a received op-06 (accessor `vms_cm_membership_coordinator_csid`, form A b[24:28] / form B b[36:40], shape-validated, honest-not-found), extracts `generation = coord_csid>>16` (WIRE-LEARNED, never hardcoded — proven by a synthetic gen-7 vector), computes `own_csid = (generation<<16)|(scssystemid & 0x3ff)`, fires `cnxman_club_learn_local_csid` → **node reaches MEMBER** (was honestly NEW). Capture-exact R1 vectors both forms; stays-NEW-when-no-coordinator-CSID preserved. 55/55, elf32-vax clean. The Sonnet-5 builder independently re-decoded the pcap (its own parser) — confirmed byte-exact, 0 false positives across 255 frames. **THE CN=3 LEVER IS IN (R1). On-hardware CN=3 now needs: boot-assembly + R4-KVM/R5-lab fire + E31 conndata (so a real VAX doesn't REJECT).**
**RESIDUALS:**
- **E30-b (lab):** high-word = generation-vs-constant (all 3 VAXes in the capture were gen 1) — a 3rd-node/re-gen capture confirms; non-blocking (OVMX reads it from the wire either way).
- **R4 flip:** `tests/qemu/test_cluster_membership.sh` still defaults `EXPECT_MEMBER=0` + documents the old premise — flip to EXPECT_MEMBER=1 needs an actual QEMU boot run (R4, lab), not a source edit.
- **Compat:** connection-manager.yaml `real-vax-join`/op-06 "not pinned" → UPDATE to grounded+implemented-R1 (done by integrator, below).
- **Observed, not built on (honest):** the first cat-01 op-01 PARAMS frame ALSO carries 0x00010001 at b[24:28] — possibly an EARLIER CSID-learn than op-06, but n=2 real PARAMS frames + offset not grounded in the PARAMS struct → deferred (a fuller capture could move the learn earlier in the join).

### E30 (original finding, for the record) — FALSIFIED + REPLACED by a real-VAX capture (op06-join-20260903.pcap)
Lab captured a genuine VAX2 rejoin (257 cat-01/op-06 frames, real MACs). **The premise was WRONG:**
- **op-06 (cat 0x01 op 0x06) does NOT carry the JOINER's SCSSYSTEMID or CSID.** It is the EXISTING member (VAX1) re-asserting ITS OWN record: sender CSID at b[24:28] (form A) / b[36:40] (form B), incarnation quadword b[28:36] (=VAX1 boot time), last-transition quadword b[36:44]. So `vms_cm_membership_find_sysid` scanning op-06 for OVMX's own sysid finds NOTHING on real traffic → the specified E30/E8 lever CANNOT fire. (The landed instrument's honest "stays NEW" comment was right.)
- **REAL CSID mechanism (byte-exact, both nodes):** `CSID = (cluster_generation << 16) | (SCSSYSTEMID & 0x3FF)`. VAX1 1025&0x3ff=1→0x00010001; VAX2 1026&0x3ff=2→0x00010002. The generation high-word (0x0001) is carried in the COORDINATOR's CSID at op-06 **b[24]**. **Actionable (buildable now, honest):** OVMX READS the coordinator's CSID high-word (generation) from the wire, then COMPUTES its own CSID = (gen<<16)|(own_SCSSYSTEMID & 0x3ff) — NOT by finding its sysid in the record. Residual: high-word=generation-vs-constant needs a 3rd-node/re-gen capture — but OVMX reads it from the wire either way, so NOT blocking.
- **COUNT-COMMIT = cat 0x01 op 0x03 COMMIT (count=1), a single decisive VAX1→VAX2 frame** (commit-time quadword b[20:28]); op-06 is strictly POST-commit; the transition finalizes through op-0a/0b/0c barrier. (Corrects "op-06 burst commits the count"; consistent w/ book corr.5.)
- **pcap:** `tests/lab/captures/op06-join-20260903.pcap`. → ROUTED TO FABLE: ratify the CSID mechanism + correct the join (FC-P3.3 CSID-learning: replace the op-06-scan instrument with generation-read + CSID-compute) + phase2 (op-03 count-commit) → then a small build item → OVMX reaches MEMBER.

### E31 — ✅ RULED (operator 2026-09-03): SEND THE REAL PROTOCOL CONSTANTS. FIX BUILDING (fc-e31-conndata). Decoded from op06-join pcap (frames 64/72):
The 16-byte `VMS$VAXcluster` connect-data at frame `[94:110]` (byte-verified, both directions):
- **`[94:98] = 01 1b 01 03`** — IDENTICAL joiner↔member = the **CM version/protocol quad**. Peer-CHECKED (p.2-25 REJECT right). Matches the pcap library's prior 148-frame census exactly.
- **`[98:105]` (7 bytes)** — node/role-dependent: **the real JOINER (VAX2) sends ALL-ZERO here**; the member sends `01 00 01 00 NN 00 01`. Meaning still unknown (§4(N) gap) — but OVMX's existing all-zero joiner form MATCHES what a real joiner sends. ✅
- **`[105:110] = 08 00 00 06 00`** — IDENTICAL both ways = tail constant. Peer-checked.
**So the ONLY reason a real VAX would REJECT OVMX's current join is that OVMX sends the version quad + tail as ZEROS instead of `01 1b 01 03`…`08 00 00 06 00`.** A real joiner's `[98:105]` IS zero (OVMX already correct there).
**Analysis:** `[94:98]`/`[105:110]` are a PROTOCOL VERSION (the CM wire-format version), NOT a node identity — OVMX's faithful CM genuinely implements this protocol, so presenting these constants is HONEST (like declaring "I speak HTTP/1.1"), not a node impersonation. OVMX's NODE identity (OVMXJ0, a VMX node) is declared elsewhere (SCSNODE / OS-identity broadcast), untouched. **OPERATOR RULING (E31):** send the grounded CM protocol constants in the connect-data (recommended — honest protocol-version + lets a real VAX ACCEPT) vs treat them as a compat-lie (send zeros, accept REJECT). Fast call now that the format is grounded; the fix is a 2-line set of `cnxman_join_cfg.conndata` once ruled.

### E50. FC-P4.6 landed the DLM requester (INV-6 structural) — the DLM rebuild's remaining WIRE opcodes need a lab capture (FC-P5.2)
FC-P4.6's requester FSM is faithful (struct dlm_req carries NO name/mode/handle/valblk/hash → cannot plumb frame→frame; every field re-read from the executive; REDIRECT re-reads; decline stops at the declining node = grant-storm cure; E49 closed — dlm_req_fsm_observe learns body[10:12]+root name). BUT these cat-0x02 wire pieces are NOT grounded and are honestly stubbed (each counted, nothing guessed):
- **Cross-node $DEQ opcode:** §4(f).1 grounds only ENQ 0x01 / CONVERT 0x07; ioctl DEQ==3 COLLIDES with the PROVISIONAL commit 0x03. `POST_DEQ` sends nothing, counts `releases_no_wire_op` → **a released cross-node lock stays held at the master until departure reaps it.** Needs a lab-grounded opcode.
- **LVB field** (cat-0x02): no grounded offset → write crossing unsent (`lvb_write_no_wire_field`); grants carry `valblk_present=0`.
- **Reply shapes** for outcomes 2/3 (REDIRECT/ASSUME) + BLKAST: no grounded parser → explicit entry points, FC-P4.8's classifier raises them only from a grounded source.
- **SS$_TIMEOUT/PATHLOST** absent in-tree → used SS$_NOTQUEUED + SS$_UNSUPPORTED; distinct SSDEF = operator call.
**→ FC-P5.2 = a DLM-traffic lab capture grounds the DEQ opcode / LVB field / reply shapes. ⭐ CAPTURABLE DURING THE R5 CN=3 FIRE: tcpdump the DLM traffic (cat-0x02) on the wire while OVMX joins → one lab session yields BOTH the CN=3 proof AND the DLM-rebuild grounding.** Then P4.8 (classifier) → P5.3/5.4/5.5 (master/directory/rebuild). NOT on the CN=3 critical path (count-commit precedes the rebuild).

### E51. ⚠⚠ 12th BUG (boot-smoke-caught): HELLO NOT TRANSMITTED — the real CN=3 blocker
Tier-1 k3s boot smoke (vaxlab-2, TCG, real 2-node VAX cluster): boot works, `SHOW DEVICE PEA0:` online, `CLUSTER_START` returns `port_up`/`SS$_NORMAL` (no `%OVMX-W-CLUSTERPORT`), VAXCLUSTER gating real, SHOW CLUSTER reads the executive — BUT **3 tcpdumps (20/40/240s) of `ether proto 0x6007` on the pod br0 saw 0 frames from OVMX's MAC** (VAX1/VAX2's own HELLOs: 15871+1443). The port reports UP but never puts a HELLO on the wire → no peer sees OVMX → the join can never start. **This is THE CN=3 blocker** — invisible to host tests (fake ops); only a booted node catches it. Distinct from E48/E49 (directory lookup). Candidates: HELLO timer (`cf_timer_arm` in vms_pe.c) not arming/firing in the REAL fork context; `exec_lan_xmit` (substrate LAN send, exec_kbackend_linux.h) stubbed/unwired in the boot image; multicast join failed silently; PEA0→ETH0 bound to the wrong iface (not the pod's br0-facing NIC). Owner FC-P0.9/P0.11. **FIX + verify on the k3s boot loop (reuse /tmp/labjoin_pod_boot_smoke.sh + labjoin harness on vaxlab-2): OVMX's MAC must transmit HELLO on br0.**
### E52. E47 diag-ioctl fix NOT verifiable on the bootable image (boot-smoke): the diag test binaries ship only in tests/qemu/Dockerfile (ovmx-ktest), not distro/Dockerfile.bootable → `%DCL-E-IVIMAGE`. Not an E47 failure — a staging gap. Follow-up: stage the diag binaries into the bootable image OR run them from the ktest image variant.

### E51 — ✅ FIXED (fc-e51 @ 6d31238c, integrated). Root: `exec_lan_open` (Linux SS14) registered dev_add_pack but never brought the netdev UP → HELLOs hit the `noop` qdisc (returns ≥0 = "success") → dropped silently. Fix = shared `exec_netdev_ensure_up()` (mirrors exec_l2_open), refuse honestly if bring-up fails. ON-WIRE PROVEN: OVMXJ1 (sysid 1986) 142 HELLO frames, 2.05s cadence, 4m49s, 0 gaps, real identity; SHOW CLUSTER/LOCAL_PORTS counter corroborates. VAXes SEE it (leg d PASS).

### E53. ⚠ NEXT ADMISSION LAYER (E51 fix exposed it): OVMX on the WRONG cluster multicast group
OVMX transmits to `ab:00:04:01:00:00` (group **00:00** = cluster group 0, auth_group=0 — no CLUSTER_AUTHORIZE staged) but VAX1/VAX2 listen on `ab:00:04:01:01:01` (group **01:01** = cluster group **257**). So the VAXes see OVMX's traffic on the wire (leg d) but do NOT admit it (wrong group → CN stayed 2, legs a/b/c FAIL). The last 2 bytes of the HELLO multicast `AB-00-04-01-<grp_hi>-<grp_lo>` = the CLUSTER GROUP NUMBER (from CLUSTER_AUTHORIZE.DAT). **To join the lab cluster OVMX needs: (a) GROUP# = 257 (0x0101, directly observed from the VAXes' multicast) — configure OVMX's cluster group → `pe_hello_multicast` computes the right group; and (b) the cluster PASSWORD/auth if the HELLO/join carries a password-derived credential the VAXes check (the CLUSTER_AUTHORIZE credential, RE-gap vms-732).** This is the CLUSTER_AUTHORIZE long-tail (vms-d21/732/098) NOW ON THE CRITICAL PATH to admission. Group# is knowable; the PASSWORD is a lab/operator fact (the vaxlab VAX cluster's CLUSTER_AUTHORIZE password). NEXT: determine the lab's group#(=257)+password (from the vaxlab VAX config or operator), wire OVMX's CLUSTER_AUTHORIZE/SYSGEN group to match, stage into the boot image, re-fire → does OVMX get ADMITTED (CN=3) or hit a further auth gap?

### E54. NetBSD `exec_lan_open` (vms_lan_netbsd.c) has the SAME missing-bring-up shape as pre-fix Linux (raised by E51 fix, not touched — no NetBSD-VAX lab evidence yet). Flag for a NetBSD boot check. Also: `tests/qemu/test_cluster_start_negctl.sh` (FC-P0.11's R4 negctl for exactly the HELLO-tx bug class) EXISTS but is NOT wired into CI (needs privileged host net) → add to a privileged/lab leg so this bug class can't recur silently.
**MEASURE-FIRST grounding (2026-09-03, no fix — gating fact unknowable without a NetBSD-VAX fire):** `exec_lan_open` binds a pfil hook to an EXISTING `ifnet` (ifunit) and never sets IFF_UP/IFF_RUNNING; `exec_lan_xmit` → `if_transmit_lock` directly. Shape DIFFERS from Linux (there OVMX owns+registers the netdev; here it borrows a kernel one), but the failure mode is the SAME CLASS and is CONFIRMED real-in-principle: `docs/research-netbsd-lan-binding.md` §4 / `if_qe.c:695-704,213` — the DEQNA hardware multicast filter is reprogrammed ONLY `if (ifp->if_flags & IFF_RUNNING)`, so on a not-RUNNING bound interface cluster **multicast rx goes dark** (group is in the ethercom list but the hw filter is never programmed) AND driver **tx drops**. THE ONE GATING FACT, ungrounded: on a NetBSD-VAX SYSKRNL boot, is qe0/ETH0: already UP+RUNNING (guest rc/DECnet/TCPIP brought it up) at cluster-open time, or must the executive bring it up? If already up → E54 is a non-issue (record the disproof). If not → `exec_lan_open` must ensure UP via the maintained path (if_up()/if_init via SIOCSIFFLAGS, NOT a raw `if_flags |= IFF_UP` poke — [[netbsd-accounting-maintained-accessor]]), mirroring Linux's exec_netdev_ensure_up. **DO NOT blind-hack bring-up** (could double-init or fight the guest's own network config). Resolve when a NetBSD-VAX cluster fire or a boot-config capture lands; capturable alongside the FC-P5.2 lab work.

### E53 — ✅ FIXED (fc-e53 @ 048b4e66, integrated). Group→MAC mapping hoisted to codec_hello (`vms_cluster_hello_mcast_build`, `ab:00:04:01:<LE16(group)>`), host-tested (257→01:01:01, 0→00:00). CLUSTER_AUTHORIZE.DAT group# staged into the boot image (Dockerfile.bootable ARG). ON-WIRE PROVEN: group=257 → OVMX HELLO to `ab:00:04:01:01:01`, byte-identical to VAX2; **VAX1 ENGAGES (930 directed HELLOs, 300s); OVMX SHOW CLUSTER/LOCAL_PORTS = channels 2, circuits 0.** CN still 2. NOT a password reject (no credential ever sent — cluster_authorize password not yet on the wire).

### E55. ⚠ NEXT GAP (E53 re-fire pinned it): channel forms but VIRTUAL-CIRCUIT never forms — `channels 2, circuits 0`
OVMX + VAX1 exchange directed HELLOs (channel discovery works, VAX1 polls OVMX 930×) but **no VC forms** — no SCS START/STACK/ACK, no SCS-ENVELOPE/SHORT frame names OVMX, no CSB for OVMXJ1 in VAX1's SDA. The channel-up→VC-formation escalation never fires. **Likely ties to FC-P0.8's own open escalation: "OVMX never emits b2 (grounded refusal); whether the joiner must INIT with b2 is unanswered — needs an R5 observation."** The e53 pcap (`tests/lab/captures/e53-group257-refire-20260903.pcap`, in-tree) HAS the 930-directed-HELLO ladder → decode it to see: does OVMX emit b2/b3/b4? does the ladder reach VERIFIED? does either side send SCS START? Then fix (emit b2 if the joiner must / wire channel-up→VC-START) + re-fire → circuits≥1 → the join proceeds. Diagnosis needs NO new lab run (pcap in-tree); re-fire only to verify. **This is upstream of the password (vms-732) — VC must form before any credential is exchanged.**

### E55 — ✅ FIXED (fc-e55 @ d679af6b, integrated). Channel VERIFY unblocked: OVMX learns the join nonce LIVE off a real peer's HELLO (was absent/zero, P0.8 gap). ON-WIRE PROVEN: nonce `ee05395b` byte-exact vs VAX token; **b2/b3/b4 ladder completes BOTH directions with BOTH VAX1+VAX2 (first ever; prior run 310 VAX HELLOs / 0 b4)**. SCS layer then BEGAN (OVMX 0x41 START + 0xb3 bulk to both VAXes; each VAX replied once — genuine bidirectional SCS, also a first). 57/57 host.

### E56. ⚠ NEXT WALL (E55 re-fire pinned it): SCS START / VC does NOT converge → still circuits 0, CN=2
Channel VERIFIED, SCS traffic flowing, but the VC never forms: OVMX sends repeated `0x41` START/config + four 1500B `0xb3` bulk frames to BOTH VAXes; **each VAX replies exactly ONCE with its own 1500B `0xb3`, then the exchange stalls** (heavily asymmetric: many OVMX sends, one reply apiece). OVMX SHOW CLUSTER shows only itself; VAX1 SDA no OVMXJ1 CSB; CN=2. **This is the SCS START/STACK/ACK convergence — the same shape as the historical [[cluster-promotion-gap]]/[[cluster-new-hang-connect-collision]] wall, but now reached with the FAITHFUL stack + a verified channel (diagnosable, not fabricated).** Two hypotheses: (1) OVMX doesn't ACK/advance off the single `0xb3` reply it gets → a completion-field bug in the SCS START state machine (P1.2 VC FSM / P2.2 SCS FSM); (2) the harness's fixed 60s post-Welcome join-wait is too short for TCG-speed 1500B bulk. **DIAGNOSE from the re-fire pcap** (`/data/training/vax/k8s-labs/vaxlab-2/logs/join-e55refire-1788460304.pcap` on the tank, in the vaxlab-2 pod — pull it, decode the 0x41/0xb3 exchange: does OVMX ack/advance off the VAX's reply?). The asymmetry (one reply apiece) favors hypothesis 1. Fix the code; the integrator orchestrates the build+re-fire (subagent-build-loop trap). Also: labjoin_booted.sh doesn't kill its own tcpdump before `wait` → idles DUR+120s (harness quirk to fix).

### E56 — ✅ FIXED (fc-e56 @ 5c7baa0c, integrated tip 5c7baa0c, 57/57). ⚠ MY BRIEF'S PREMISE WAS WRONG — corrected by frame census. The "0xb3 bulk / one reply apiece" is NOT SCS: it's §4(k) padded channel-SIZE-verify HELLOs (abs30=b3, abs31=0x00, not the 0x13 SCS format byte). Census of all 135 frames the VAXes sent OVMX in the re-fire: b2×2, b3×53, b4×80 — **ZERO SCS frames ever**; OVMX sent 242 byte-identical 0x41 STARTs into silence. So the wall is UPSTREAM of SCS: **the VAX never opened its circuit / never sent its own round-0 START.** Controlled pair `ovmx-5fe-channel-formed-20260728.pcap` (the MEMBER-reaching build): same b2→b3→b4, and 10ms after b4 VAX1 emits its round-0 0x41 START unprompted 18×. Byte-diff of OVMX's directed HELLO good-vs-bad = node identities + live tick + **abs 47–67**: good build carries a 21-byte span, E56 build carried ZEROS. Census over 10 captures: 11403/11575 HELLOs carry it (VAX1/2/3 + the MEMBER build) — node-INDEPENDENT ⇒ a discovery-frame FORMAT field, not an identity claim; zeroing it was never honest omission. FIX = `pe_learn_disc_format()` learns the span off the first real peer's discovery frame (the E55 nonce mechanism; no constant baked, no meaning claimed for any of the 21 bytes — Rule 8). Spec grounded: cluster-protocol-spec.md §4(a).2 + §4(a) abs 47/64/65 unknown→GROUNDED. **NEXT: single-variable re-fire (E56 alone) to confirm the VAX now sends its round-0 START + whether the VC forms — before stacking E57.**

### E57. ⚠ TEED UP (predicted by E56's census, NOT yet confirmed on the wire — do not fix until the E56 re-fire shows the VAX's round-0 START arriving). OVMX's outgoing 0x41 START body still carries **abs 72–79 software-version = 8 zero bytes** and **abs 95 CLUSTER_CREDITS = 0**. The MEMBER-reaching OVMX build sent `"VMX V0.1"` + `10`; every VAX sends `"VMS V7.3"` + `10` (§4(g), 28/28). Expect the VAX to reject/ignore OVMX's START on these the moment its round-0 START starts arriving (i.e. after E56 works). **NOT echoable from a peer** — `"VMS V7.3"` would be MASQUERADE ([[honest-os-identity-broadcast]] rules: broadcast OVMX's REAL version). FIX (mine, groundable): (a) expose OVMX's real version to kernel-core — `ovmx_identity.h` is USERLAND, so add a version field to `VMS_IOCTL_SYSGEN_LOAD` (⚠ new ioctl field → "N places" trap) or a kernel-core version accessor; (b) add a `params_valid` flag set by `cluster_sysgen_load()` so `cluster_credits`=10 is distinguishable from "never loaded"=0. Both DISCLOSED in the GAPS block in vms_pe.c, unfilled. Fix only after the E56 re-fire confirms this is the real next gate.

### E56 RE-FIRE (single-variable, vaxlab-2, TAG e56refire-1788463417) — ✅ FIX CONFIRMED ON THE WIRE; big leap, not yet CN=3
The abs 47–67 discovery-format fix WORKS: **the VAX now opens its circuit.** Both VAX1 AND VAX2 emit their OWN round-0 0x41 SCS START UNPROMPTED (VAX1 t=17.30102 vs its b4 t=17.30092 = +0.10ms, BEFORE OVMX's reply t=17.30128; VAX2 likewise) — genuinely VAX-initiated, matching the ovmx-5fe control's shape (TCG-compressed vs bare-metal ~10ms). **Frame census VAX→OVMX: 3849 frames / 1373 SCS-family (0x41/0x5b/0x48/0x7b)** vs the failing baseline's 135 frames / 0 SCS. Sustained rich two-way SCS the whole run. **VC: PARTIAL** — OVMX PEA0 `SHOW CLUSTER/LOCAL_PORTS` = `channels 2, circuits 2`, both VAX CSIDs (1025,1026), STATUS=`NEW` (never advances to MEMBER). VAX1 `SHOW CLUSTER` now lists a 3rd row `OVMXJ1 | ? | ` (heard, software unknown, blank status) — but SDA CSB list still only VAX1+VAX2 (no OVMXJ1 CSB). **CN still 2.** Legs a/b/c FAIL, d/e (cap-denied) PASS. pcap on tank: `/data/training/vax/k8s-labs/vaxlab-2/logs/join-e56refire-1788463417.pcap` (24430 frames). Not committed in-tree (CN≠3 gate).

### E57 — ⚠ NOW CONFIRMED BYTE-EXACT ON THE WIRE (was predicted; the E56 re-fire proved it is the real next gate). NEXT FIX.
OVMX's outgoing 0x41 START to VAX1: **abs 72–79 software-version = `00×8`** (should be OVMX's real `"VMX Vx.y"`), **abs 95 CLUSTER_CREDITS = `00`** (should be `10`). VAX1's own START in the SAME exchange: abs72-79 = `"VMS V7.3"`, abs95 = `0x0a`. This lines up EXACTLY with VAX1 rendering OVMXJ1's software as `?` and forming no CSB → the plausible reason STATUS stalls at NEW and CN stays 2. **FIX (dispatched, o5): send OVMX's HONEST version** ([[honest-os-identity-broadcast]] — real `VMX Vx.y` from ovmx_identity.h, NOT masquerade `"VMS V7.3"`) + **real credits=10 guarded by a `params_valid` flag** (so 0=never-loaded is distinguishable). Plumbing: ovmx_identity.h is USERLAND → carry version down via a new field on `VMS_IOCTL_SYSGEN_LOAD` (⚠ new ioctl field → "N places"/NetBSD-parity trap) or a kernel-core version accessor; `cluster_sysgen_load()` stores version + sets params_valid; the vms_pe.c START-body builder fills abs72-79 + abs95 from stored state (never a constant/echo). **⚠ HONEST-FIRST LADDER:** if the real VAX REFUSES the honest `"VMX Vx.y"` (still `?` / no CSB on the next re-fire), the compat-lie `"VMS V7.3"` becomes authorized as LAST-RESORT per [[honest-os-identity-broadcast]] — but only AFTER the wire proves refusal; do NOT pre-emptively masquerade.

### E57 — ✅ FIXED (fc-e57 @ b340b411, integrated tip b340b411, 57/57 + identity 10/10 + unit 106/106 + kif_caller_census + includes-gate OK + NetBSD cross-vax ELF32 ILP32-clean). Honest OVMX identity now on the START wire. SSOT symbol = **`OVMX_CLUSTER_SW_VERSION`** (ovmx_identity.h — its own header names it "the SCS START/config wire field"; NOT product-version `"V0.6-10"` which is 11B & overflows the 8B field, NOT `ovmx_compat_version()` `"VMS V7.3"` which would be the forbidden masquerade). Value had DRIFTED (a V0.6-10 executive was broadcasting a `"VMX V0.1"` claim) → bumped to honest `"VMX V0.6"` (8B). `params_valid` flag (set ONLY by cluster_sysgen_load(), never an ioctl field — anti-fabrication) gates credits=10; unloaded → honest zeros + `vc_sw_version_absent`/`vc_credits_absent` counters (no fabricated default). SYSGEN_LOAD "N places" all updated (vms_ioctl.h struct 104→112B + ioctl nr 0x…68→0x…70; NetBSD vms_lock_nb.h mirror; devtab; vms_cluster.h; vms_cluster_sysgen.c/h accessors; vms_pe.c/pe_fsm.c builder; ovmx_init.c userland caller). identity_ssot_gate CAUGHT a stray literal in a comment → removed (not allowlisted). New test_identity_ssot.sh §5 pins OVMX_CLUSTER_SW_VERSION to OVMX_PRODUCT_VERSION major.minor + drift-fires. ⚠ 3 CARRIED: (1) **the `V0.1`→`V0.6` DIGIT change is untested vs the real VAX** — `"VMX V0.1"` is the only byte string empirically MEMBER-accepted; VAX renders the field verbatim (no evidence it parses digits), so honest V0.6 is expected fine, but IF the next re-fire still shows `?`/no-CSB, revert is a 1-line SSOT change. (2) CLUSTER_CREDITS>255 = honest-omit+count (not truncate) — flag if a clamp is wanted. (3) **RELEASE-CUT COUPLING** → E59.

### E57 RE-FIRE (single-variable, vaxlab-2, TAG e57refire-1788467379) — ✅ version half LANDED PERFECTLY; ❌ credits half did NOT reach the wire → CN still 2. The version fix is 100% on the wire: OVMX's outgoing 0x41 START `abs72-79 = "VMX V0.6"` **530/530 frames**, and **VAX1 SHOW CLUSTER now renders `OVMXJ1 | VMX V0.6 |`** (was `?`) — the VAX sees OVMX's honest identity. BUT `abs95 CLUSTER_CREDITS = 0x00` **530/530** (VAX1's own = 0x0a). STATUS still NEW, no OVMXJ1 CSB in VAX1 SDA, CN=2. Channel-verify + SCS exchange otherwise unchanged from E56. **The credits-fill path is broken while the version-fill path works** — isolated single defect. pcap on tank: `/data/training/vax/k8s-labs/vaxlab-2/logs/join-e57refire-1788467379.pcap` (24646 frames). → E60.

### E60. ⚠ NEXT FIX (dispatched, o5): CLUSTER_CREDITS still 0 on the START wire. The E57 fix landed the version (sw_version via SYSGEN_LOAD) but `cluster_credits` reaches the vms_pe.c builder as 0. **⭐⭐ FAITHFULNESS GATE (operator's #1 recurring catch [[executive-backed-not-wire-plumbing]]): the credit value MUST be READ FROM ACTUAL EXECUTIVE STATE — the count of receive buffers OVMX has genuinely allocated for the VC / its SCS flow-control receive window — NOT a stuffed constant `10`.** If OVMX has 0 receive buffers allocated for the VC, then 0 is HONEST and the real fix is buffer allocation (a deeper VC-setup gap), NOT advertising a fake 10. Diagnose: (a) why cluster_credits=0 when sw_version landed (plumbing: was a credits value ever sourced/passed? the E57 "N places" added sw_version to the ioctl + ovmx_init fills it, but credits may never have been populated); (b) ground what SCS "credits" faithfully IS from VAXcluster Internals (~/cluster: SCS flow-control / send-credit / receive-buffer chapter) + the OVMX VC/port receive-buffer code. FIX = advertise the genuine allocated receive-credit count from executive state; if that's structurally 0 today, the fix is to allocate the VC receive buffers and advertise the real count. Commit on fc-e60; integrator orchestrates build+re-fire.

### E60 — ✅ FIXED (fc-e60 @ 825dd501, integrated tip 825dd501, 58/58 + unit 107/107 + identity/sysgen 11/11 + includes-gate OK + Linux vms.ko clean + NetBSD-VAX ALL PROOFS PASSED ELF32-clean). Executive-backed credits, done right. ROOT CAUSE (two defects): (a) VALUE NEVER SOURCED — `sysgen_read_param()` reads ONLY the persisted store; the shipped `distro/rootfs/.../SYSEXE/OVMXVMSSYS.PAR` has NO CLUSTER_CREDITS record (only 7 params) → absent→−1→memset-0→committed "valid, 0" → 0x00 on wire, `vc_credits_absent` never bumped (invisible). Version worked because sw_version comes from the compiled-in SSOT, not the store. (b) NOTHING BACKED IT — no per-circuit receive-buffer commitment existed. GROUNDING (VAXcluster Internals, Rule-8 page-cites): p.2-43 the credit IS the receive-buffer count (SYSAP requests N buffers = extends N send-credits); p.2-45 one MFREEQ per port + per-CDT share; p.2-43 bank-analogy invariant. DERIVATION: grant = min(SYSGEN request, buffers still unpromised in the port's REAL pool), capped ≤255. FIX: `pe_credit_ledger{pool,reserved}` + init/available/reserve/release (grants can't exceed pool); `cf_rx_pool_bufs()` reads what `cf_create` REALLY allocated (port MFREEQ); reserve@formation/release@close; `vc_fill_advertised()` writes `vc->recv_credit_max` + counts every zero grant; absent CLUSTER_CREDITS → SYSGEN table default (`SYSGEN_DEFAULT_CLUSTER_CREDITS`, `%OVMX-I-NOPARAM`). Renamed `pe_identity.cluster_credits`→`credits_requested` (a field named for the wire that no longer reaches it invites the next plumbing bug). **Wire byte fully traceable, no constant: frame ← vc->recv_credit_max ← pe_credit_reserve() ← f->credit.pool ← cf_rx_pool_bufs() ← exec_zalloc.** Lab node pool 64/request 10 → advertises 10 BECAUSE IT HAS 10. Test `test_credit_is_the_reservation_not_the_request` drives the same request over pools 64/7/4/0, asserts parsed abs-95 == recv_credit_max == credit.reserved each time (a future request→frame shortcut reds HERE not on the cluster). No ioctl change → no N-places. Caught+fixed a real NetBSD `-nostdinc` bug (`f != NULL`→`f ?`). **NEXT: build+re-fire — the CN=3 milestone test.**

### E61. ⚠ REAL BUG, own item + operator decision (surfaced by E60, NOT fixed — wider than cluster): the store-vs-table gap zeroes MANY params. `TIMVCFAIL`, `NISCS_MAX_PKTSZ`, `LOCKDIRWT`, `QDSKVOTES`, `DISK_QUORUM`, `MSCP_SERVE_ALL`, and **`MSCP_LOAD`** are ALL absent from the shipped `.PAR` → read 0 every boot. **`MSCP_LOAD` default is 1 → the MSCP server is silently NOT loaded on any node booted from the shipped image** (contradicts [[mscp-serve-actual-volume]]). AUTHENTIC FIX (VMS uses a param TABLE with defaults + a store for overrides): extend E60's table-default-on-absent to ALL params — merge-on-load in `sysgen_read_param`/`sysboot.c` (preferred, authentic) OR regenerate the seed store (brittle). ⚠ verify whether any of these (esp. NISCS_MAX_PKTSZ) affects membership BEFORE assuming E60 is the last join field — the E60 re-fire will tell. File as rd item.

### E62. SCS SYSAP-level credits are still bare constants (surfaced by E60, own item): `CNXMAN_JOIN_DIR_CREDITS 3`/`MSCP_CREDITS 4`/`CM_CREDITS 4` (vms_cnxman_join_fsm.h) → scs_listen/scs_connect with NO allocation ledger behind them — same anti-pattern one layer up (per-CDT, not per-VC). Apply the E60 ledger shape to the SYSAP layer. Not blocking initial membership.

### E60 RE-FIRE (single-variable, vaxlab-2, TAG e60refire-1788471537) — ✅ credits LANDED (both identity fields now fully honest on wire); ❌ CN still 2 — but a NEW THRESHOLD: the VC now OPENS. abs95 credits = `0x0a` **506/506** (was 0x00), abs72-79 = `"VMX V0.6"` 506/506. Boot log: `%OVMX-I-NOPARAM, CLUSTER_CREDITS absent … using SYSGEN default (10)` — honest (node genuinely has 10 buffers). **NEW BEHAVIOR: VAX1 console emitted UNSOLICITED `%PEA0, Port has Closed Virtual Circuit - REMOTE NODE OVMXJ1`** — a VC can only CLOSE if it OPENED (E57/E56 never got this; they plateaued silently). So OVMX's VC now OPENS on the VAX side. But it OPENS→CLOSES→RETRIES: 107 distinct OVMX START episodes evenly across the ~1550s run (~1 per 17-18s). VAX1 SDA still only VAX1+VAX2 CSBs, SHOW CLUSTER `OVMXJ1 | VMX V0.6 | ` (blank status). Byte-diff of OVMX's START vs VAX1's: NO further OVMX-side zero beyond version+credits (both fixed) — so the wall is DOWNSTREAM of START, not a zeroed START field. pcap on tank: `/data/training/vax/k8s-labs/vaxlab-2/logs/join-e60refire-1788471537.pcap` (23740 frames). → E63.

### E63. ⚠ NEXT WALL (dispatched, o5): VC OPENS then CLOSES in a retry loop (107×) — the SCS sequenced-handshake convergence, no CSB, CN=2. This is the memory's [[cluster-new-hang-connect-collision]] / [[cluster-promotion-gap]] CN=3 lever, now reached with the FAITHFUL stack. **STRONG PRIOR GROUNDING (docs/cluster-protocol-spec.md §4d/§4f/§4g/§4h + docs/design-cluster-join-choreography.md, run-1030 analysis) — TEST as hypothesis, don't assume:** VMS's SCS retransmit-clear/reconfiguration counts ONLY the recv_ack PIGGYBACKED on a SEQUENCED frame (send_seq≠0, env 0x4b/0x5b), NOT standalone 0x48 credit-returns. Root cause proposed: OVMX's sequenced recv_ack to the NON-COORDINATOR member VAX1 FREEZES one short because OVMX stops sending sequenced frames to VAX1 after the initial handshake — it barrier-talks only the COORDINATOR and does NOT maintain the ongoing sequenced DLM-rebuild exchange (cat-0x01 op-0x05 lock/resource rebuild; cat-0x02 op-0x0d DLM records) with VAX1 that a real joiner runs continuously (ref vax3-2to3: 8377 sequenced frames to VAX1). Frozen recv_ack → VAX1 reconfiguration never completes → VC-reset timeout → the 107× retry loop. FIX likely = extend the WORKING per-peer coordinator op-0x0d echo (CM_RSP_DLM, scs_dlm.c) to ALL members (verify — may not be net-new DLM). INV-6: echo/reflect VAX1's rebuild records at NL mode with VAX1's own value blocks, NEVER fabricate held lock state. Diagnose from the e60 pcap (one open→close cycle: where does OVMX's recv_ack to VAX1 freeze; does OVMX send sequenced frames to VAX1 post-handshake). Commit fc-e63; integrator orchestrates build+re-fire.

### E63 — ✅ FIXED (fc-e63 @ 89fc0da2, integrated tip 89fc0da2, 58/58 + unit 107/107 + includes-gate OK + NetBSD ELF32-clean; revert reds test_codec_vc 46/208). **Prior hypothesis REFUTED by the pcap** (measure-first win): it was NOT "recv_ack one short / member-vs-coordinator" — recv_ack from BOTH VAXes is **ZERO** (neither ever acked ANY of OVMX's 8550 sequenced frames; neither sent a 0x48 credit-return). OVMX never reaches the DLM phase; wall is UPSTREAM of any member distinction. ROOT CAUSE: OVMX's `CONNECT_RSP` had **zeros in the transport-counter span abs 36..55** — `vms_scs_seq_stamp()` wrote only abs 32/34/44; `vms_scs_fsm.c::ctrl_prepare`'s own comment admitted "no generic builder for the rest yet". Census: abs 36/38/52/54 = zero in **0 of 239,981** reference sequenced frames. Malformed CONNECT_RSP → VAX can't establish the sequenced channel → never acks → bare ~17s VC timeout → 99-episode retry loop (VAX1 realloc'd SCS$DIRECTORY Con.ID each cycle; NO disconnect/reject/diag code). FIX: `seq_stamp_span()` in the port codec writes the whole abs36..55 span at transmit + re-stamps on retransmit. EXECUTIVE-BACKED: abs 40/48 = the SAME `pe_vc.recv_seq` read that fed abs 32; abs 38/52/54 = node-independent protocol constants the codec's honesty rule already admits (credit-return builder writes them); abs 36 = grounded FLOOR 1 (76.1% corpus, 622/622 credit class, 0/239981 zero) w/ the 2/3 residue declared an RE gap (Rule 8, no generator inferred). Cites: cluster-protocol-spec.md §4(d)/§4(h)(1a,3,4) + new §4(h)(4b); VAXcluster Principles pp.2-30/2-31, 2-43, Fig 2-14. **NEXT: build+re-fire — does the VAX now ACK OVMX's sequenced frames + establish the channel.** ⚠ CARRIED (agent left, out of E63 scope — revisit only if the re-fire needs them): (1) abs 36 residue 2/3 in 24% of real frames UNDECODED (writes floor 1; next candidate if re-fire stalls with all else correct; may need material we don't hold); (2) OVMX `ACCEPT_REQ` carries credit 3 where golden carries 1; (3) OVMX issues `VMS$DISK_CL_DRVR` CONNECT_REQ BEFORE the SCS$DIRECTORY dialogue completes (golden defers to seq 12) — premature, plausibly benign.

### E63 RE-FIRE (single-variable, vaxlab-2, TAG e63refire-1788476287) — span-fill NECESSARY but NOT SUFFICIENT; recv_ack STILL 0, retry churn WORSE (152 vs 107). E63's CONNECT_RSP abs36-55 fill is correct+kept (a well-formed CONNECT_RSP needs it) but didn't move recv_ack: max recv_ack VAX1→OVMX = 0, VAX2→OVMX = 0; no 0x48 CREDIT ever from either VAX. OVMX ctrl census: CONNECT_REQ/CONNECT_RSP/ACCEPT_REQ, **never ACCEPT_RSP(3)**, no APPLICATION(10)/DLM. VAX ctrl census: **CONNECT_REQ(0) ONLY** — the VAX never advances past its own directory lookup. TWO E63 residuals RESOLVED: abs36 residue 2/3 NOT observed (every frame incarn-echo=1 → E63's floor-1 was RIGHT); ACCEPT_REQ credit=3 confirmed 100% (vs golden 1). NEW: OVMX CONNECT_RSP `conid_local` (abs68-71) = all-zero. → E64.

### E64. ⚠ NEXT WALL (dispatched, o5) — THE REAL BLOCKER, now cleanly pinned: **OVMX races ahead of its own SCS$DIRECTORY handshake / connects the WRONG SYSAP for membership.** OVMX's CONNECT_REQ SYSAP-name census: **711× `('MSCP$DISK','VMS$DISK_CL_DRVR')` (PREMATURE disk-class-driver connect) vs only 24× `('SCS$DIRECTORY','SCS$DIR_LOOKUP')`.** The VAX's own CONNECT_REQ is EXCLUSIVELY `('SCS$DIRECTORY','SCS$DIR_LOOKUP')` (1031×) — it never gets past directory lookup because OVMX never completes it. From t=17.1s OVMX pairs `ACCEPT_REQ(SCS$DIR_LOOKUP/SCS$DIRECTORY)` with `CONNECT_REQ(MSCP$DISK/VMS$DISK_CL_DRVR)` in the SAME burst, every ~0.6-1s. **For CLUSTER MEMBERSHIP the SCS connection needed is the CONNECTION MANAGER (CNXMAN/VMS$VAXcluster-class), NOT the MSCP disk-class driver** — OVMX is issuing the disk connect prematurely (and likely instead of the CNXMAN connect). FIX = make OVMX follow the golden VAX connection CHOREOGRAPHY: (a) complete the SCS$DIRECTORY / SCS$DIR_LOOKUP dialogue first; (b) connect the correct membership SYSAP (connection manager); (c) DEFER the MSCP$DISK/VMS$DISK_CL_DRVR connect until AFTER membership (golden defers to seq 12); (d) fill `conid_local` in CONNECT_RSP (all-zero now — the Con.ID exchange, VAXcluster Internals Fig 2-14). GROUND in docs/design-cluster-join-choreography.md + cluster-protocol-spec.md §4 + the golden capture's VAX sequence + VAXcluster Internals (SCS directory service + connection manager + Con.ID exchange). This may clear multiple downstream walls at once (it's the connection-establishment order, not one frame field). INV-6: connection IDs/state READ from executive SCS state. Commit fc-e64; integrator orchestrates build+re-fire.

### E64 — ✅ FIXED (fc-e64 @ 3c1373a8, integrated tip 3c1373a8, 59/59 + unit 108/108 + full 235/235 + includes-gate OK 71 + guest-payload PASS + elf32-vax ALL PROOFS PASSED + Linux vms.ko clean; regression-red PROVEN by breaking each rule). **My brief was HALF WRONG (agent corrected it):** the golden VAX3 join (vax3-2to3 capture) issues the `MSCP$DISK` connect at ss=6 as STEP 3 of the join (gated on its lookup HIT) — it is NOT post-membership; "defer MSCP" would have BROKEN golden order. `vms_cnxman_join_fsm.c` was already correct (ordering locked by test_cnxman_join.c). REAL CAUSE: `src/kernel-core/vms_mscp_cl.c` (FC-P7.1 disk class driver) ran an INDEPENDENT 1s beat from CLUSTER_START (`cl_sweep_peers`→`cnxman_disk_client_connect`) over every system-with-a-circuit, NO directory lookup, NO membership gate, concurrent with the join → a SECOND originator on the shared per-channel send_seq. OVMX's first sequenced frame to VAX2 was ss=1 MSCP$DISK (the beat), its own join dir connect landed BEHIND at ss=2 → shared-sequence deadlock (spec §4(L)). FIX: new pure TU `vms_mscp_cl_conn_fsm.{c,h}` [6 states × 5 events], Rule1 = lookup-before-connect (silence→unanswered++, never absence, p.2-51), Rule2 = while not MEMBER the sweep originates NOTHING (not even an inquiry — takes a seq slot). Membership SYSAP = **`VMS$VAXcluster`** GROUNDED from real-VAX1 SDA SHOW CONNECTIONS (`sda-scs-extract-vax1.txt`) + wire + p.2-25/2-51 (OVMX already spells it, `cnxman_join_name_vaxcluster`). Four-places done (5 build lists + netbsd Dockerfile). **conid_local=0 was a FALSE ALARM** (golden op=1 ECHO also carries lc=0; OVMX op=2 ACCEPT_RSP mints a real lc). → E65 for the VAX1 leg.

### E65. ⚠ THE VAX1-LEG BLOCKER (E64 fixed the VAX2/MSCP-poison leg only; recv_ack=0 on VAX1 is a SEPARATE cause). ROUTED TO FABLE for the grounding ruling (agent declined to guess — the operator's #1 hot-button). Byte-diff OVMX op=1 ECHO vs golden: **abs 32..71 IDENTICAL**; only the connect-family marker at **abs 72-75** differs: golden VAX3/VAX1/VAX2 = `00 00 01 00`, MSCP-connect variant = `02 00 01 00`, **OVMX (executive) = `00 00 00 00` on EVERY connect-family frame**. DECISIVE: the `ovmx-760-MEMBER-achieved-20260730.pcap` (the OVMX specimen a real VAX ADMITTED to the cluster) sends `00010000`/`00010002` there. Low word (abs 72-73) varies + demonstrably not load-bearing; **abs 74-75 = `0x0001` INVARIANT across 3 real-VAX captures + the 1 accepted-OVMX run** (stronger grounding than E63's abs-36 floor, which had residue). Also: OVMX NUL-pads the 16-byte trailer abs 108-123 where every real node blank-pads (`0x20`) when supplying no connect data. `vms_cluster_codec_scs.h` currently documents abs 72-75 as caller-supplied "no asserted meaning" — so emitting a value there must UPDATE that honesty rule with the grounding. FABLE QUESTION: does a 4-capture node-independent invariant (incl. an accepted-OVMX run) count as GROUNDED-emittable (the E63 corpus-constant precedent) or a forbidden template constant (INV-6/[[executive-backed-not-wire-plumbing]])? Per ruling → implement E65 (emit abs74-75 executive-side w/ census+cite, blank-pad trailer) + BATCH with E64 for the next re-fire (both needed: E64=VAX2 leg, E65=VAX1 leg — single-variable value is gone since both were isolated from the SAME existing pcap).

### E65 FABLE RULING (INV-6 dissolved — the field is NAMED, read-from-state, not a template constant). Dispatched to impl (o5). abs 72-75 are two published `$SCSDEF` fields (grounded from the project's OWN admitted oracle `captures-lab2/vms0fe-vaxlab-3-oracles-20260806.txt` L85-146 — the source spec §5 + the codec op-verb enum already cite; message LENGTHS match byte-exact): **abs 72-73 = `SCS$W_MIN_CR`** (SYSAP Minimum Send Credits arg, VAXcluster Principles p.2-44), **abs 74-75 = `SCS$W_STATUS`** (SCS's own outcome code; `SCS$K_STNORMAL=1`). Layout offset 0 = abs 72 (MTYPE abs 60 = −12). **THE VAX1-LEG ROOT CAUSE:** OVMX sends STATUS=0 on CONNECT_RSP; 0 is not a defined $SCSDEF value → VAX1 reads the echo as a FAILED connect (p.2-48), releases its CDT, re-polls forever (517 CONNECT_REQ, never a CONNECT_RSP/ACCEPT_RSP back to OVMX). Per-op census (Fable's own byte-exact re-read, 5 real-VAX captures + accepted-OVMX-760, zero residue): STATUS = **1** on op 0 CONNECT_REQ / 1 CONNECT_RSP / 3 ACCEPT_RSP / 4 REJECT_REQ; **0** on op 2 ACCEPT_REQ (OVMX ALREADY correct — keep); 0-init/1-match on op 6 DISC_REQ. MIN_CR = 0 on directory/CNXMAN connects (honest "no floor"), 2 on MSCP$DISK (record as vms_mscp_cl's REAL need, don't copy). **NEW BLOCKER (missed by byte-diff): OVMX CONNECT_RSP zeroes abs 76-79** where real nodes echo the answered CONNECT_REQ's DST_PROC name first-4 (`SCS$`/`MSCP`/`VMS$`/`SCA$`, 148/148; = SCS$T_DST_PROC[0:4], CON_RSPL=22). **Trailer abs 108-123: blank-fill 0x20 when no connect data** (100% of no-data connects; OVMX NUL-pads today). IMPL SHAPE: codec replace `uint8_t marker[4]`→`uint16_t min_cr; uint16_t status;` + `#define VMS_SCS_ST_NORMAL 1u` (+6 codes); `ctrl_prepare()` sets status from a per-op table (FSM outcome, NOT a capture) + min_cr from the CDT; `ctrl_send_echo()` (vms_scs_fsm.c:1035) fills tail4 from listen->local_name[0:4]; blank-fill trailer at scs_fsm_connect/listen_reset; add min_credits to scs_connect_args + scs_fsm_accept, store on CDT; delete the "genuine omission" comment vms_scs_fsm.c:465. ⚠ the directory op-10 body's `marker` is a DIFFERENT field at the same abs — LEAVE it. Residue: REJECT/DISC reason-code word (all specimens 0/1, undecodable) = RE gap; STATUS→retry mechanism inferred (p.2-48 + census) — the batched E64+E65 re-fire tests it. INV-6: every value read-from-state (CDT/FSM/listen), zero copies.

### E65 — ✅ IMPLEMENTED (fc-e65 @ 2a009ad9, integrated tip 2a009ad9, 59/59 + unit 108/108 + full host 225/225 + includes-gate OK 71 + NetBSD ELF32 ALL PROOFS PASSED + Linux vms.ko clean; regression-red proven per rule). Fable ruling implemented exactly, every field READ-FROM-STATE (INV-6, no capture-copy): abs74-75 STATUS from `ctrl_status_for_op(op,cdt)` per-op table (op0/1/3/4=STNORMAL=1, op2=0, op6=`cdt->disc_peer_matched?1:0`); abs72-73 MIN_CR from `ctrl_min_cr_for_op` (cdt->local_min_send_credits ops0/1/2, cdt->peer_min_send_credits op3); abs76-79 op1 tail4 = `listen->local_name[0:4]`; trailer abs108-123 blank-0x20 via `conndata_set()`. codec `marker[4]`→`min_cr;status;` + VMS_SCS_ST_NORMAL + 6 codes; directory op-10 marker left untouched. **MSCP MIN_CR = 0 not oracle's 2** — grep shows vms_mscp_cl has NO credit machinery → honest minimum is "no floor"; emitting 2 would be an ungrounded capture constant (INV-6). Tests parse REAL fixtures + assert on the frame OVMX puts on the wire (not table-vs-itself). ⚠ ONE FORCED BEHAVIOR CHANGE (from item 2, faithful): op3 stores the answered ACCEPT_REQ's MIN_CR in `cdt->peer_min_send_credits` (the same field p.2-44's "dangerously low" test uses) → `credit_dangerously_low()` now includes the peer's real floor (no change vs OVMX peers=0; vs real VMS MSCP$DISK=2 the type-8 threshold rises by 2 = the faithful p.2-44 rule); test_scs_fsm_credit.c premise "not grounded on wire" is DEAD, now asserts the stronger fact. RE gaps declared: REJECT/DISC reason word (Fable's), connect-data <16B no specimen. **BOTH LEGS NOW FIXED (E64 VAX2 shared-seq poison + E65 VAX1 STATUS=0-invalid) — build+re-fire is the CN=3 milestone test.**

### E65 RE-FIRE (batched E64+E65, vaxlab-2, TAG e65refire-1788483810) — fields LANDED but wall moved DOWN a layer; recv_ack STILL 0. E65 CONNECT_RSP now byte-perfect: abs74-75 STATUS=0x0001 **4061/4061**, abs76-79 DST_PROC echo=`SCS$` **4061/4061** (both were 0). E64 confirmed: premature MSCP$DISK connect GONE — OVMX CONNECT_REQ census now `('SCS$DIRECTORY','SCS$DIR_LOOKUP')×24` ONLY (was 711 MSCP + 24 dir). Both sides cleanly locked in SCS$DIRECTORY. BUT: max recv_ack VAX1→OVMX=0, VAX2→OVMX=0 (unchanged EVERY run); VAX retransmits identical CONNECT_REQ (send_seq pinned ≤2 across 515+) never advancing to ACCEPT_REQ; OVMX's recv_ack TO the VAX maxes at 1 while VAX send_seq maxes at 2 (OVMX one behind); no 0x4b sequenced/DLM either direction; 124 retry episodes; VAX1 unsolicited "Closed Virtual Circuit - OVMXJ1"; CN=2. **Agent's pin: the two facts (VAX send_seq never climbs + VAX never acks OVMX) point at the TRANSPORT/ACK LOOP itself, NOT the CONNECT_RSP body** (which is now correct). → E66.

### E66. ⚠ NEXT WALL (dispatched, o5) — THE TRANSPORT/VC SEQUENCED-MESSAGE ACK LOOP (abs 0-55 port span, below SCS). recv_ack VAX→OVMX = 0 has PERSISTED through E63 (CONNECT_RSP counter span abs36-55) + E65 (SCS body abs72+) → it is a SEPARATE, deeper defect in the PE/VC sequenced-frame ACK handling (vms_pe_fsm.c / vms_pe.c / the go-back-N VC FSM from E10/FC-P1.9), NOT the control-message content. Symptom: VAX sends CONNECT_REQ (send_seq≤2), never acks OVMX (recv_ack=0), keeps retransmitting → OVMX's ACK of the VAX isn't landing AND/OR OVMX's sequenced frames aren't being accepted by the VAX at the transport level. **KEY DIAGNOSTIC LEVER: `tests/lab/captures/ovmx-760-MEMBER-achieved-20260730.pcap` is an OVMX build the VAX ACKED INTO MEMBERSHIP** (the strawman — but transport ack is transport ack, a valid golden OVMX reference). BYTE-DIFF current-OVMX sequenced frames vs 760's at abs 0-55 (recv_ack abs32-33, send_seq, the VC/NISCA ack + flow-control fields) → find the transport-field regression/gap that makes the VAX ack 760 but not current. Ground vs VAXcluster Internals PEDRIVER/NISCA sequenced-message ACK + VC datagram format. INV-6: ack/seq values read from VC state. Commit fc-e66; integrator orchestrates build+re-fire.

### E66 — ✅ FIXED (fc-e66 @ 6d3f5d73, integrated tip 6d3f5d73, 59/59 + unit 108/108 + full 235/235 + includes-gate OK 71 + NetBSD ELF32 ALL PROOFS PASSED + Linux clean; negative-control re-baking reds test_codec_vc+test_pe_vc). **THE recv_ack=0 ROOT CAUSE — and it CORRECTS E63's wrong model.** The transport field gating the VAX's ACK is **abs 36 = the §4(i).B NODE-INCARNATION ECHO on EVERY class** (NOT the "small message count, floor 1" E63's §4(h)(4b) modelled). OVMX's CONNECT_REQ is byte-identical to VAX1's own on the wire EXCEPT MACs/LAVC-addrs/local-Con.ID — the sole discriminator: OVMX **baked `1`** at abs36 on its `0x4b/0x5b/0x7b` sequenced + `0x48` credit-return frames, where it must ECHO the peer's LIVE-ADVERTISED incarnation (VAX1+VAX2 advertise **8**, 1743 directed HELLOs). Members DISCARD frames with the wrong echo → recv_ack frozen 0 AND their credit-returns/CONNECT_REQ never retire → ~430× retransmit at frozen send_seq. OVMX's `0x41` START/STACK/ACK passed only because those ALREADY carried the real echo (that's why the VC opened). PROOF (`tools/cluster/incarnation_census.py`): af2 capture — VAX1 advertises 1→2→3, joiner's abs36 walks LOCKSTEP on all 3 classes; e65 live — 6/6 directions "abs36 == what THAT peer advertised to me", incl. the asymmetric VAX1↔VAX2 pair (a message-count can't do that). 760 was acked by LUCK (virgin pod, every advertisement=1). FIX: `vc_incarnation(vc)` ← `pe_vc.echo_incarnation` ← `pe_channel.peer_incarnation` ← peer's real directed HELLO [78:80], feeds all 3 seq_stamp sites + credit builder; **zero REFUSED (E_INVAL, 0/239981), counted `vc_no_incarnation`, never substituted**; `VMS_SCS_SEQ_MSG_COUNT` deleted. Spec §4(h)(4c) corrects §4(h)(4b). Cites pp.2-12/2-15/2-16/2-30/2-31/2-43. UNILATERAL (OVMX's own HELLO correctly advertises 1 per §4(i).B "joiner's own always carries 1") — needs no member change; re-fire is the test. **This is why recv_ack was stuck through E60/E63/E65 — the abs36 echo, now executive-backed off the peer's live advertisement.**
(⚠ E63 §4(h)(4b) abs-36 floor-1 SUPERSEDED by E66 §4(h)(4c); E63's transport-counter-span fill for the OTHER abs36..55 bytes stays valid.)

### E66 RE-FIRE (vaxlab-2, TAG e66refire-1788488599) — ✅✅ MAJOR BREAKTHROUGH: the incarnation-echo fix WORKS. Multiple first-evers: **recv_ack NON-ZERO both VAXes** (VAX1→OVMX=12, VAX2→OVMX=18; was 0 EVERY prior run) — the transport ACK loop functions. abs36 now stamps live incarn=9 across all classes (was baked 1). **VAX now sends CONNECT_RSP/ACCEPT_RSP + real APPLICATION(10) traffic** both VAXes reach ACCEPT_RSP(3)+APPLICATION — 0x4b sequenced MSG up to 204B — genuine SYSAP/DLM exchange, a first. **Retry episodes collapsed 124→1** (clean first-attempt connect). **A real OVMXJ1 CSB now forms in VAX1 SDA ANALYZE/SYSTEM** — first ever. BUT CN still 2: CSB sits at `State: 09 wait`, `Flags: long_break,send_ext_status`, SHOW CLUSTER `OVMXJ1 | VMX V0.6 | BRK_NEW`, poll status NEW throughout (was absent). pcap on tank: `/data/training/vax/k8s-labs/vaxlab-2/logs/join-e66refire-1788488599.pcap` (12775 frames). → E67.

### E67. ⚠ NEXT WALL (dispatched, o5) — THE PROMOTION GAP (NEW→MEMBER), reached FAITHFULLY at last. The full connect handshake succeeds (START→CONNECT→ACCEPT→APPLICATION→MSG, 2-3× per VAX, incl. a 204B message) in ~1s (t≈17.4-18.4), **then OVMX goes COMPLETELY SILENT for the remaining ~1600s** — no DISCONNECT, no retry. OVMX RECEIVES the VAXes' post-connect APPLICATION/membership messages (recv_ack advances) but does NOT drive the ongoing promotion protocol → VMS detects a break, CSB → `long_break` (Reconn.time `0x0001C66E`), sits in reconnect-wait. **This is the [[cluster-promotion-gap]] / [[cluster-new-hang-connect-collision]] the memory long flagged as the CN=3 lever — now genuinely reached (live CSB, real app traffic), not the strawman's fabrication.** DIAGNOSE: decode OVMX's post-connect silence vs the golden joiner's continuation (vax3-2to3 config burst at seq 14/15 → MEMBER; the barrier; the vote/config exchange that advances NEW→MEMBER). Pin what CNXMAN application/membership frames OVMX should send in RESPONSE to the VAXes' APPLICATION messages but doesn't. Decode the CSB state machine (State 09 wait / BRK_NEW / long_break / send_ext_status) from VAXcluster Internals (connection-manager membership + config/vote + the CSB states). Fix the CNXMAN membership FSM to process the incoming membership messages + drive the config/barrier/promotion to MEMBER. INV-6: membership state read from executive. Prior grounding: docs/design-cluster-join-choreography.md + cluster-protocol-spec.md §4 + [[cluster-barrier-5of12-empty-payload]] + [[cluster-promotion-gap]]. Commit fc-e67; integrator orchestrates build+re-fire.

### E67 — ✅ FIXED (fc-e67 @ eaad5095, integrated tip eaad5095, 59/59 + unit 108/108 + includes-gate OK 71 + kif census PASS + Linux vms.ko clean + NetBSD ELF32 ALL PROOFS PASSED; negative controls red 18/18). **THE 1600s SILENCE ROOT CAUSE — the join FAILED AT STEP 1.** Defect (1): a glue thunk in vms_cnxman.c returned the executive's `SS$_NORMAL`(=1) verbatim where the pure FSM ops contract is 0/nonzero → `join_send_lookups()` issued both directory inquiries (went out, answered HIT) but read both SS$_NORMAL as REFUSALS, counted zero issued, `join_h_start` failed at step 1; `[FAILED]` empty table row → every subsequent directory answer + inbound connection + op-0x01 counted-and-dropped; `cnxman_join_drive()` won't restart a non-IDLE join = the entire silence. **SAME FAMILY AS E43** ("beds return 0 → R1/R2 tests stay green" — the recurring glue-status trap). Defect (2) structural: the join could only speak on a connection IT opened (`cnxman_join_connect_req()` discarded the member's `peer_conid`; join_emit_cm refused without cm_conid/cm_open). PROMOTION SEQUENCE (golden vax3-2to3): once VMS$VAXcluster connection OPEN, joiner emits **cat-01 op-0x14 MODEL (msg#1) + op-0x01 PARAMS (msg#2)** immediately, then op-0x02 CONFIG → op-03/05 answered 0x81 → op-06 burst → cat-04 barrier. **op-0x01 PARAMS carries VOTES/EXPECTED_VOTES/QDSKVOTES/LOCKDIRWT — exactly the CSB's p.7-23 node-parameter block; without it VAX1's OVMXJ1 CSB can't be filled → OVMX can't be COUNTED** (CSB `State: wait`=p.7-24 lost-connectivity/§7.10 reconnect timer; BRK_NEW=MEMBERS-class CSB rendering). FIX: `cnxman_fsm_rc()` named SS$_→0/nonzero translation on all 5 ops (+ trap armed for ops.send/respond); `CNXMAN_EV_CM_ACCEPTED` + `cnxman_join_cm_accepted()` + `join_open_cm()` ADVERTISES on the ADOPTED (member-opened) connection instead of opening a 2nd. Every emitted value executive-backed: MODEL←cfg.model, PARAMS VOTES←cl->params.votes (real SYSGEN), body[0:8]←cnxman_envelope_stamp() on the CSB's real dialogue counters; unowned = counted-never-acted (cm_other_member/cm_already_held, no fabrication). test_cnxman_join 151 checks (adopt→burst→disk-walk→op-0x02→ADMIT→COMMIT→XITGO). → re-fire is the test.
**⚠ CARRIED ESCALATION → FC-P3.3 (per-peer join, architect):** the golden joiner runs op-0x14/op-0x01 + MSCP$DISK connect with EVERY member (VAX3 did both VAX1+VAX2); this single-target join drives ONE member + counts the others `cm_other_member`. Generalizing to per-peer + the tie-break for a true simultaneous VMS$VAXcluster open (NO capture grounds which side yields — this node yields nothing, invents nothing) is an FSM redesign, NOT smuggled into E67. **The E67 re-fire may promote with one member but need per-peer for full CN=3 in the 2-node cluster — the re-fire tells us.**

### E67 RE-FIRE (vaxlab-2, TAG e67refire-1788492708) — fix passed host tests but did NOT manifest at runtime (BEDS-vs-RUNTIME GAP, the E43/E47 family the E67 agent itself flagged). Both VAXes independently completed CONNECT→ACCEPT→APPLICATION and sent OVMX a **PARAMS (cat=CONFIG/0x01 op=PARAMS/0x01, len=204)** at t=16.150 (VAX2) / 16.171 (VAX1) — both connections reached the SAME point. **But OVMX emitted ZERO CONFIG-category (cat=0x01) frames back** — no MODEL/PARAMS/CONFIG/COMMIT, no op-06/09/0b barrier, either side. CM traffic confined to t≈14.9-16.2 then stops (only b3/b4 HELLO heartbeat continues ~1538s). CSB still `State 09 wait`, Flags send_ext_status (no member), Quorum/Votes **0/0**; SHOW CLUSTER `OVMXJ1|VMX V0.6|NEW`; both VAXes finally OPCOM `%CNXMAN, timed-out lost connection to OVMXJ1`. NOT a clean per-peer asymmetry (both reached PARAMS). ⚠ Also: a burst of **len=108/124 frames DID NOT decode** to a recognized cat/op (agent refused to invent — Rule 8) — THE KEY CLUE (may be OVMX's malformed promotion attempt). Also noted: VAX2's FIRST connection was REJECTED (VAX2→OVMX REJECT_REQ t=14.937 → OVMX REJECT_RSP → OVMX DISCONNECT_REQ) before the working one formed t=16.145. pcap on tank: `join-e67refire-1788492708.pcap` (span 1618s). → E68.

### E68. ⚠ NEXT — WHY the E67 runtime doesn't emit the promotion response despite green host tests (BEDS-vs-RUNTIME gap; ⚠ recurring — the cluster host tests use beds that return SS$_NORMAL/0 and MASK the real executive path; E43/E47/E67 all this shape → [[p1-milestone-real-executive-harness]] [[release-qa-not-ci-green]]). Dispatched (o5). TWO branches from the KEY CLUE: **(A)** decode the len=108/124 undecoded frames — direction + full byte layout; are they OVMX-originated? do they carry a promotion cat/op (MODEL 0x14 / PARAMS 0x01 / CONFIG) at ANY offset? byte-diff vs the golden joiner's MODEL/PARAMS. If OVMX IS emitting but MALFORMED (wrong cat/op offset, wrong struct — the E67 emit has a codec bug) → fix the frame. **(B)** if OVMX emits NOTHING recognizable → the E67 adoption/advertise path (cnxman_join_cm_accepted / join_open_cm) isn't FIRING at runtime though host tests pass; trace what runtime condition differs (does the real accept path call cnxman_vc_opened→cnxman_join_cm_accepted? is the join FSM in the state the advertise needs? did the initial VAX2 REJECT leave the join non-IDLE so drive() refuses?). If unresolvable from wire+code (no executive console log — ovmx_init.c:1399), build MINIMAL executive observability: a CNXMAN-join FSM state-transition + emit-attempt trace readable post-run (diag ioctl ring buffer / the simplest thing) so the next re-fire SHOWS the runtime FSM — this is the systemic unlock for the beds-vs-runtime wall. Commit fc-e68; integrator orchestrates.

### E68 — ✅ FIXED (fc-e68 @ b4a844d2, integrated tip b4a844d2, 59/59 + unit 108/108 + gate OK 71 + kif census PASS + Linux clean + NetBSD ELF32 ALL PROOFS PASSED; negative controls red 16/16; test_cnxman_join 182 checks incl. 4 replaying the pcap in measured order). **The len=108/124 undecoded frames were INNOCENT** — SCS directory-layer (len124=connect-family control CON_REQ/ACCP_REQ; len108=APPLICATION carrying SCS$DIRECTORY body incl. literal "NOT PRESENT HERE"), decoded as noise only because CM offsets abs80/81 were applied. OVMX emitted ZERO 204B CM frames all run (census confirmed). **RUNTIME ROOT CAUSE the beds masked:** t=14.9372 OVMX CON_REQ VMS$DISK_CL_DRVR→MSCP$DISK to VAX2 → t=14.9374 **VAX2 REJ_REQ** → OVMX h_rx_reject → cnxman_join_closed → join_h_closed (mscp_conid matches, mscp_walk_done==0) → **`join_fail(PATHLOST)` → terminal FAILED**; then t=16.15/16.17 both VAXes open VMS$VAXcluster, OVMX accepts, `CNXMAN_EV_CM_ACCEPTED` hits the empty `[FAILED]` row → dropped → 1585s silence. E67's adoption cell is correct in 8 states; runtime reached the 9th (FAILED) 1.2s before membership was offered. FIX: `join_disk_client_gone()` — a refused/lost MSCP$DISK connection is NON-FATAL (counted mscp_rejected/mscp_lost, drive continues to step 4); VMS$VAXcluster reject stays terminal. GROUNDED 3 ways, decisively by **golden vax3-2to3: the joiner's VMS$VAXcluster to VAX2 opened at t+30.369, its MSCP$DISK connect 0.46s LATER, and the reference joiner ITSELF REJ_REQs both members' inbound MSCP$DISK connects while its join proceeds** — an MSCP reject is normal. INV-6: zero wire values changed, control-flow only. → re-fire test.
**⚠ CARRIED ESCALATIONS:** (1) **FAILED is TERMINAL — nothing restarts a join** (`cnxman_join_drive` refuses non-IDLE) → ANY single transient during the drive = permanent silence; join-restart policy is an architecture decision → **Fable / new item [needs a join-supervisor/restart FSM].** E68 removes the MSCP trigger but not the fragility. (2) per-peer/tie-break for simultaneous VMS$VAXcluster open → FC-P3.3. (3) **BEDS-vs-RUNTIME is now 2 consecutive walls (E67/E68); diagnosis worked only because the pcap happened to expose the state** — the E68 agent recommends + offered to build a read-only `(state,event,emitted-op,rc)` diag-ring behind an E47-pattern ioctl → TEED UP: build it IF the E68 re-fire stalls at a non-wire-visible point (measure-first).

### E68 RE-FIRE (vaxlab-2, TAG e68refire-1788496312) — E68 changed runtime behavior (no longer terminal-silent) but OVMX STILL emits zero CM promotion frames; reason now WIRE-INVISIBLE. Positive: SCS-family traffic now continues the ENTIRE ~1620s run (was hard stop at t≈16.2 in E67) — E68's MSCP-reject-non-fatal keeps the join loop ALIVE. But: (a) OVMX now RE-SENDS a `SCS$DIRECTORY/DIR_LOOKUP` CONNECT_REQ to VAX2 every ~10.24s ×148, answered only by a bare CREDIT+short(len80) MSG, never re-reaching ACCEPT — the VAX2 directory connect retry-loops without completing; (b) VAX1 sent ONE PARAMS (t=18.4; VAX2's didn't appear this run) and OVMX NEVER answered with its own MODEL/PARAMS/CONFIG. **Zero OVMX-originated cat=0x01 CM frames all run.** CSB still State-09-wait / votes 0/0 / SWVers blank; SHOW CLUSTER `OVMXJ1|VMX V0.6|NEW`; both VAXes OPCOM timed-out. **The promotion-drive gap is a RUNTIME STATE INVISIBLE FROM THE WIRE — 3rd consecutive beds-vs-runtime wall (E67/E68/here).** → E69: BUILD EXECUTIVE OBSERVABILITY (the diag ring is now warranted, measure-first threshold crossed).

### E69. ⚠ EXECUTIVE OBSERVABILITY (dispatched, o5) — the systemic unlock for the beds-vs-runtime wall. No executive console log exists (ovmx_init.c:1399), so 3 straight promotion walls have been diagnosed from the wire by luck; the drive gap is now wire-invisible. BUILD a read-only `(seq, t, state, event, emitted-op, rc)` RING in the CNXMAN join/CM path (vms_cnxman_join_fsm.c + the SCS→CNXMAN dispatch), dumped via an E47-pattern read-only diag ioctl (ahead of the registration gate; mind N-places), + a small DCL-callable userland dumper staged in the bootable image that prints the ring to SYS$OUTPUT so the labjoin console capture (ovmx-node-*.log) records it, + the harness issues that DCL after the join-wait. INV-6: observability ONLY, changes NO wire behavior, no fabricated state. Instrument the transitions that answer: when VAX1's VMS$VAXcluster ACCEPT/`CNXMAN_EV_CM_ACCEPTED` fires, what state is the join FSM in, does the advertise fire, what rc; is the join stuck in the VAX2 directory-connect retry loop blocking CM-accept handling; where exactly the MODEL/PARAMS emit is gated off. Host tests for the ring; commit fc-e69. Integrator orchestrates the build+re-fire that CAPTURES the runtime FSM → then E70 diagnoses the drive gap WITH that data.

### E59. RELEASE-ENG follow-up (mine, release-eng lane — NOT blocking the join): E57's new gate ties `OVMX_CLUSTER_SW_VERSION` to `OVMX_PRODUCT_VERSION` major.minor (test_identity_ssot.sh §5 fires on drift). So a version bump at release-cut must ALSO bump OVMX_CLUSTER_SW_VERSION in step — same shape as the existing ovmx_identity.h + os-release in-step tag discipline (vms-8328 gate). `tools/cut-release.sh` only reads OVMX_PRODUCT_VERSION today. FOLD the cluster-sw-version bump into the in-step release check so a future cut can't red the gate. The §5 test is the safety net until then.

### E58. Lab-build-to-tank mechanism is OUT-OF-TREE (reproducibility gap). The e55/e56 builds use a HAND-WRITTEN Job manifest (custom, fixed name, privileged pod on k3s-worker, in-pod dockerd `--insecure-registry 192.168.2.43:30500`, `git checkout <sha>` from origin, `docker buildx build --load -f distro/Dockerfile.bootable`, `docker cp` to a **hostPath `/tank`→`/data/training/vax`** at `k8s-labs/<name>/`). The stock `tools/k3s/job-template.yaml` does NOT mount the tank, so this bypasses it. E56 build proved the branch-on-origin path works (no LAN-git needed once pushed). FOLLOW-UP: fold the hostPath-tank mount into `tools/k3s/job-template.yaml` as an opt-in flag + a short lab-build runbook, so the build+re-fire is fully in-tree-reproducible. Not blocking the join work.

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

### E10. VC gap-break under loss → RULED a fidelity bug; FC-P1.9 go-back-N LANDED (Fable, design §3.2.5)
**recv_anchored deletion RATIFIED by Fable (§3.2.5): §4(i).A grounds recv_seq=0 as the port's formation anchor; no re-formation case exists; corrected failure shape (gap→ladder exhaustion→loud break) is right. No follow-up.**

**Status: FIXED, and the FC-P2.2 half is now LANDED.** FC-P1.9 implemented the
ruling on top of FC-P1.3. **FC-P2.2 landed the SCS half**: `scs_fsm_vc_down(f, peer,
reason)` walks that SB's CDT queue and closes every CDT with `SCS_CLOSE_PATHLOST`,
discards the ledgers, fails every Credit Wait entry with path-lost through the
SYSAP's `send_failed`, and calls `closed()`. R1 test
(`tests/cluster/host/test_scs_fsm_vcdown.c`) asserts each clause on an SB carrying
four connections + that ZERO frames went on the wire during/after the break.
FC-P2.4's glue only points `pe_upper_ops.vc_down` at it + maps the reason to `SS$_PATHLOST`.
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
- **FIX = FC-P1.9 — LANDED.** `vms_pe_fsm.c`: `h_vc_rx_gap()` discards + re-acks
  (no break); `vc_ring_oldest`/`vc_ring_next_after`/`vc_resend_from` go back N in
  SEQUENCE order from the oldest unacked entry (a failed transmit STOPS the run —
  pushing the tail past a frame that never left would recreate the gap);
  `h_vc_retransmit` breaks on `PE_VC_DOWN_RETRANSMIT_EXHAUSTED` when the OLDEST
  entry has spent `PE_VC_RETRANSMIT_TRIES` (25, OVMX design value seeded from
  §4(k)); `PE_VC_DOWN_SEQ_GAP` deleted (value **1 retired, never re-used**; the
  other reasons are now PINNED at their FC-P1.2 numbers and EXHAUSTED = 7).
- **The cadence default changed and the relation is load-bearing:** with no
  SYSGEN value the retransmit interval is `TIMVCFAIL / (TRIES + 1)` so the whole
  ladder completes INSIDE TIMVCFAIL and exhaustion — not the silence timer — is
  the detector that names the reason. An explicitly configured
  `vc_retransmit_ms` still wins; if its ladder outlasts TIMVCFAIL, TIMVCFAIL
  fires first and says so.
- **The `vc_down` seam is `pe_upper_ops.vc_down` (vms_pe.h §4), NOT a new
  `pe_ops` entry.** `pe_ops` is the SUBSTRATE seam (send/timers/clock/log);
  `pe_upper_ops` is the SCS-facing one and already declared `vc_down(peer,
  reason)`, which the FSM raises from `vc_notify_down()`. FC-P1.9 made it
  load-bearing and documented the FC-P2.2 contract on it rather than adding a
  second path for one event. **FC-P2.2 binds this.** `reason` is an
  `enum pe_vc_down_reason`, not an SS$_ status — the pure FSM has no SS$_
  definitions (§3.2.2); the GLUE maps it.
- **⚠ SECOND DEFECT FOUND BY THE ACCEPTANCE, FIXED HERE — the ANCHOR.** FC-P1.2's
  `recv_anchored` ("the first sequenced message on a circuit anchors the counter")
  is a **capture-scanner** property: §4(h)(4a) describes it for "a node attaching
  to a circuit already carrying traffic", which a port that FORMED the circuit is
  not. Under break-on-gap it looked harmless; under go-back-N it SILENTLY LOSES
  DATA — a lost first message makes the second one the anchor, this node
  acknowledges a message it never received and the sender's ring releases it
  (measured: 46 of 48 delivered, seed 3). Deleted, on §4(i).A's grounding: "the
  post-START SCS VC resets to `send_seq = 1` on both sides ... 0 residuals", so
  `recv_seq = 0` at formation IS the anchor. `PE_VC_SEQ_ANCHOR` and
  `pe_vc.recv_anchored` are gone. **A peer that opened at some other number now
  gaps and is broken by its own exhausted ladder — loud, with a reason.**
- **R2 acceptance (tests/cluster/sim/test_sim_vc.c), both green:** 48 pipelined at
  10 % loss × 8 seeds → 48/48 delivered, **0 out of order**, **0 VC breaks**,
  retransmits 9–31, gaps 5–21; 100 % one-way loss → exactly 25 retransmissions,
  exactly ONE break, `vc_down` raised once with RETRANSMIT_EXHAUSTED, then
  re-formation. In-order is proved by reading each delivered frame's `send_seq`
  back through the codec (`SIM_M_UPPER_OUT_OF_ORDER`), not by counting arrivals.

### E11. RESOLVED by FC-P0.9 — pure `pe_fsm_view_project()` now exists
FC-P0.9 added `pe_fsm_view_project()` (the pure port-view assembler) to
`vms_pe_fsm.c` with its own R1 test (`test_pe_view.c`). The sim's `sim_dump.c`
can now switch `dump_port()` from reading raw `struct pe_fsm` counters to this
pure projection (a follow-up cleanup, not blocking).

### E13. FC-P1.9 merge-care: P0.9 also edited vms_pe_fsm.c (raised by integrator)
FC-P1.9 (go-back-N) branched from feat/cluster-executive BEFORE FC-P0.9
integrated, and both edit `vms_pe_fsm.{c,h}` (P0.9 added `pe_fsm_view_project`;
P1.9 rewrites gap-handling + adds a `vc_down`/`pe_ops` hook). The regions are
likely disjoint but the merge needs a careful read. Also **FC-P1.6** (vms_pe.c VC
glue) must consume P1.9's `vc_down` pe_ops hook — dispatch P1.6 only AFTER P1.9
lands so its ops table is built against the final `pe_ops` shape.

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
