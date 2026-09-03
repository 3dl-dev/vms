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
