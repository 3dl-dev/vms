# OVMX cluster-join dir-CLIENT choreography (vms-760, NEW→MEMBER)

**Status:** grounded + byte-verified 2026-07-29; implementation in progress.
**Provenance:** clean-room — `formation-clean-2node.pcap` wire observation + public
OpenVMS docs only (CLAUDE.md Rule 8). Every offset/value below was decoded by
Con.ID pair from the clean 2-node formation (joiner MAC `08:00:2b:94:ca:47`),
**not** trusted from any agent's frame ordinals (those drifted ~1 and mislabeled
frames — the orchestrator re-derived from the wire).

## Why OVMX stalls at NEW

OVMX reaches `SHOW CLUSTER` status NEW by opening only the `VMS$VAXcluster` VC and
sending the add-member burst; the member credits it but never reciprocates. The
grounded reason: a real joiner first acts as a **directory + disk CLIENT** — it
opens its own `SCS$DIRECTORY` connection, **resolves each SYSAP via a lookup before
connecting to it**, and connects `MSCP$DISK` before the VC. The member reciprocates
the add-member config on the joiner VC (→ MEMBER) only once that full connection-set
is present. OVMX presents 0 `VMS$DISK_CL_DRVR` frames vs the clean joiner's 41.

Two OVMX attempts regressed and pinned the mechanism:
- **760b** (own dir connect, then jumped straight to the VC connect): the VC connect
  was presented before its SYSAP was resolved → member only *echoed* it (op=1, lc=0),
  never sent the op=2 response that supplies its handle → VC never bound, retransmit
  forever, never even NEW.
- **760mscp** (MSCP connect with no prior lookup): the unprocessable connect on the
  **shared** per-channel `send_seq` created an in-order **hole** → member's cumulative
  `recv_ack` froze at 2 → the next VC connect was never accepted → below NEW to blank.

**Invariant:** the per-channel `send_seq` is shared across all Con.ID pairs and must
stay **contiguous**; exactly one join-drive frame outstanding at a time; a connect is
issued **only after** its directory lookup returns a HIT; retransmits **reuse** the
frame's `send_seq` (never advance). Any frame the member cannot process at that point
poisons the sequence.

## The dir-CLIENT dialogue (byte-verified, joiner handle `0x4e630007` ↔ member `0xe2dc0008`)

All frames ethertype 0x6007. Absolute offsets (frame byte 0 = Ethernet dst):
`msgtype@30`, `recv_ack@32` (le16), `send_seq@34` (le16), `op@60` (le16),
`remote_conid@64` (le32), `local_conid@68` (le32), `marker@72` (le32),
`name@76` (16B SYSAP), `result@92` (16B).

| step | dir | mt | SCA len | op | seq | rc | lc | name@76 | result@92 | marker@72 |
|------|-----|----|---------|----|----|-----|----|---------|-----------|-----------|
| connect-req | J→M | 0x5b | 110 | 0 | 1 | 0 | `0x4e630007` | `SCS$DIRECTORY` | `SCS$DIR_LOOKUP` | `0x00010000` |
| echo | M→J | 0x5b | 66 | 1 | — | `0x4e630007` | 0 | — | — | `0x00010000` |
| response | M→J | 0x5b | 110 | 2 | — | `0x4e630007` | `0xe2dc0008` | `SCS$DIR_LOOKUP` | `SCS$DIRECTORY` | 0 |
| **confirm** | J→M | 0x5b | **62** | **3** | 2 | `0xe2dc0008` | `0x4e630007` | *(none)* | *(none)* | `0x00010000` |
| lookup TAPE | J→M | 0x5b | 94 | 10 | 3 | `0xe2dc0008` | `0x4e630007` | `MSCP$TAPE` | zeros | 0 |
| resp MISS | M→J | 0x5b | 94 | 10 | — | `0x4e630007` | `0xe2dc0008` | `MSCP$TAPE` | `NOT PRESENT HERE` | 1 |
| lookup DISK | J→M | 0x5b | 94 | 10 | 4 | `0xe2dc0008` | `0x4e630007` | `MSCP$DISK` | zeros | 0 |
| resp HIT | M→J | 0x4b | 94 | 10 | — | `0x4e630007` | `0xe2dc0008` | `MSCP$DISK` | `MSCP$DISK` (echo) | 1 |
| lookup VC | J→M | 0x4b | 94 | 10 | 7 | `0xe2dc0008` | `0x4e630007` | `VMS$VAXcluster` | zeros | 0 |
| resp HIT | M→J | 0x4b | 94 | 10 | — | `0x4e630007` | `0xe2dc0008` | `VMS$VAXcluster` | `01 1b 01 03 …06 00` | 1 |

Discriminators (byte-verified):
- **request vs response** on op=10: a REQUEST has `marker@72==0` and `result@92`=zeros
  (and `rc`=peer handle, `lc`=own handle); a member RESPONSE has `marker@72==1`,
  `result@92` filled, `rc`=**our** handle (`0x4e630007` on the wire; OVMX's own-dir
  handle `0x4F580008` in OVMX's run).
- **HIT vs MISS**: `result@92 != "NOT PRESENT HERE"` ⇒ HIT. Marker is 1 for **both**
  hit and miss — do **not** use marker to classify, and never test `result==name`
  (the `VMS$VAXcluster` HIT returns a binary blob, not the name).

## Full join order (each connect gated on its lookup HIT)

On one contiguous shared `send_seq`, exactly one outstanding, stop-and-wait:

1. own `SCS$DIRECTORY` connect-req (op=0) → wait member echo(op=1)+response(op=2), learn member dir handle
2. dir **confirm** (op=3, new builder) — fire-and-forget (member bare-ACKs 0x48)
3. lookup `MSCP$TAPE` → wait MISS
4. lookup `MSCP$DISK` → wait HIT
5. `MSCP$DISK` connect (`scs_connect_build_mscp_request`, done) → wait member echo+response, bind
6. lookup `VMS$VAXcluster` → wait HIT
7. `VMS$VAXcluster` VC connect (`send_joiner_connect_request`, done) → wait member echo+response, bind
8. add-member burst (`cm_send_config_burst`, done) → **member reciprocates 204-byte config on the joiner VC ⇒ MEMBER**

Coexistence: throughout, OVMX also keeps answering the member's **separate** dir
probe as a dir-SERVER (member opens its own `SCS$DIRECTORY` connect later; distinct
Con.ID pair — OVMX server handle `0x4F580007` vs client handle `0x4F580008`). The
two directory connections never collide; the Con.ID pair alone disambiguates. This
refutes the earlier "own-dir suppresses the member probe" idea — the clean joiner is
a dir-client AND the member still opens its own probe.

## Implementation notes (corrections applied)

- **Seq oracle is RELATIVE, not the clean joiner's absolute values.** OVMX advances
  contiguously via `scs_seq_advance`; it does not emit the clean joiner's intermediate
  MSCP frames, so its VC connect lands at whatever contiguous seq comes next — **not**
  the clean joiner's `seq=10`. Success criterion = contiguous seq, no gap, one frame
  outstanding, member `recv_ack` never freezes. Hard-forcing absolute values would
  reopen the 760mscp hole.
- **Per-lookup seq storage-and-reuse** must be added: `send_own_dir_lookup` currently
  advances the seq every call and stores nothing. The stop-and-wait retransmit of the
  three lookups must reuse a stored seq, mirroring `own_dir_req_seq`/`mscp_req_seq`/
  `joiner_req_seq` — never re-call the sender (which would advance and open a hole).
- New builder `scs_dir_build_connect_confirm` (op=3, 62-byte SCA / 76-byte frame,
  Con.IDs bound, marker `0x00010000`, no names).
- Sequencer state in `peer_state`: `join_step` enum + outstanding-step seq + retx
  timer; response handler keys on `rc==own-dir handle && op@60==10 && marker@72==1`,
  affirmative = `result != "NOT PRESENT HERE"`.
- Keep the dir-SERVER responder (branch b2) and member-opened VC path unchanged.

## Live results (2026-07-29) — sequencer works 6/8; stalls on the ESTABLISHED member

Implemented as a stop-and-wait state machine gated behind `OVMX_JOIN_SEQ` (default
OFF so the proven VC-first NEW path is preserved — Rule 9). Against the live golden
2-node lab (`d94-760seq*.pcap`) the sequencer drove cleanly:

1. own `SCS$DIRECTORY` connect → member echo+response (OWNDIRBOUND) ✓
2. op=3 confirm ✓
3. lookup `MSCP$TAPE` → member MISS ✓
4. lookup `MSCP$DISK` → member HIT ✓ → lookup `MSCP$DISK` #2 (the clean joiner sends
   two; the member's `recv_ack` advances per lookup as expected) ✓
5. `MSCP$DISK` connect (seq 6, **byte-identical to clean sca35** outside identity/seq
   — verified) → **STALL**: the member never accepts it. Its `recv_ack` freezes at 5
   (the second DISK lookup), never acking the connect at seq 6.

Root cause (grounded, live): this is an **ESTABLISHED** 2-node cluster, not the fresh
1→2 **formation** of `formation-clean-2node.pcap`. The established member, right after
receiving OVMX's MSCP connect, opens **its own** `SCS$DIRECTORY` probe to OVMX
(`0x3566000d`) — i.e. it resolves the joiner before accepting its disk-client connect
— whereas the fresh-formation member accepted the identical connect immediately (its
own probe came much later, sca77). OVMX's answer to that probe (echo seq7 + response
seq8) sits **behind the seq-6 hole** the un-accepted connect created, so neither side
advances: a mutual-resolution deadlock specific to the established case.

## The established-join is MEMBER-DRIVEN (grounded 2026-07-30, `af2-firsttimer-established-20260728.pcap`)

The repo already had the missing reference: `~/vax/cluster/captures/af2-firsttimer-established-20260728.pcap`
— a first-timer (raw HW MAC `08:00:2b:78:56:b9`, fresh SCSSYSTEMID) joining the
**established** cluster node VAX1 (`aa:00:04:00:01:04`). Byte-decoded from the wire by
Con.ID pair (offsets §"The dir-CLIENT dialogue" above), not from frame ordinals. The
capture holds three join cycles (incarnation echo `abs22`/`abs36[0]` = 1, then 2, then
3 as the node re-forms); the analysis below is the clean incarnation-1 join at t≈143 s.

**The decisive difference from `formation-clean-2node.pcap`: against an established
member, the MEMBER drives admission — the joiner is a SERVER first.** Byte-verified
order (joiner handle `8fd_0007/8/9`, member `356b0009`/`3553000a`/`355b0008`):

| t (s) | dir | frame | note |
|-------|-----|-------|------|
| 143.104 | J↔M | 0x41 START ×6, ss=1 ra=0, incarnation=1 | symmetric, round-0 only, completes in ~2 ms |
| 143.7524 | **M→J** | op=0 `SCS$DIRECTORY` connect (`356b0009`) | **member opens the FIRST directory connect — TO the joiner** |
| 143.7526 | J→M | op=1 echo, op=2 response (`8fd20007`) | joiner answers as dir-SERVER |
| 143.7545 | M→J | op=3 confirm | the CONNECTOR (member) sends op=3 |
| 143.755 | M→J | op=10 lookup `MSCP$TAPE` → MISS | **member queries the JOINER's directory** |
| 143.756 | M→J | op=10 lookup `MSCP$DISK` → **HIT** (joiner serves it) | joiner must present `MSCP$DISK` |
| 143.758 | M→J | op=10 lookup `VMS$VAXcluster` → **HIT** | joiner must present the VC |
| 143.7579 | **M→J** | op=0 `MSCP$DISK` connect, **ss=7**, res `VMS$DISK_CL_DRVR` | **member connects as DISK CLIENT of the joiner** |
| 143.7584 | **M→J** | op=0 `VMS$VAXcluster` VC connect, **ss=8** | **member opens the cluster VC — ~0.5 ms AFTER the MSCP connect** |
| 143.759 | J→M | op=2 response on the VC (`8fd10009`) | joiner accepts the member's VC |
| 143.891 | J→M | joiner opens ITS OWN dir connect + lookups + `MSCP$DISK` connect | reciprocal client half, **~133 ms later** |
| 145.591+ | J↔M | 190-byte config burst on the VC (`355b0008`↔`8fd10009`) | CCSTART/config exchange ⇒ **MEMBER** |

**Answer to the vms-760 question ("does the member want VC/NEW before the disk-client
connect?"): NO.** The established member drives **both** connects itself — `MSCP$DISK`
connect (ss=7) **first**, then the `VMS$VAXcluster` VC connect (ss=8) ~0.5 ms later, a
single member-initiated admission burst. Neither waits on the other; there is no
"reach NEW then connect the disk" ordering. The load-bearing fact is the opposite of
what the joiner-driven sequencer assumes: **the joiner must SERVE the member's drive**
— answer the member's directory connect, serve `MSCP$DISK` **and** `VMS$VAXcluster`
lookups as HITs, and ACCEPT the member's `MSCP$DISK` and VC connects — all BEFORE it
opens its own reciprocal client half.

**Why OVMX stalls (now fully explained).** OVMX's `OVMX_JOIN_SEQ` sequencer is a
**joiner-DRIVEN** choreography (own dir connect → lookups → *OVMX* fires the MSCP$DISK
connect). That matches the 1→2 **formation** reference but collides with an
**established** member that wants to drive: the member opens its own dir probe to
resolve OVMX first (`0x3566000d`), and OVMX's un-accepted client MSCP connect sits as a
shared-`send_seq` hole → mutual-resolution deadlock. The sequencer solves the wrong
case.

**Grounded fix (server-first; test on the live lab, `OVMX_JOIN_SEQ` OFF path):**
1. **Serve `MSCP$DISK` as AFFIRMATIVE** in the dir-SERVER lookup responder. Today
   `scsd.c` sets `lp.affirmative = (name == "VMS$VAXcluster")` only (scsd.c ~1765) →
   OVMX answers `MSCP$DISK` "NOT PRESENT HERE", so the member never opens its disk
   connect. Every real node MSCP-serves its system disk; OVMX must present `MSCP$DISK`.
2. **Accept the member-initiated `MSCP$DISK` connect** (op=0 → OVMX echo op=1 →
   response op=2, binding the Con.ID pair; member = `VMS$DISK_CL_DRVR` client of OVMX's
   `MSCP$DISK`). No member-initiated MSCP accept path exists yet (the `mscp_*` state is
   for OVMX's own *client* connect).
3. Keep answering the member-opened VC connect (already reaches NEW) and exchange the
   190-byte config burst on it.
4. **Then live-test whether server-only (1–3) reaches MEMBER**, or whether OVMX must
   also open its own reciprocal `MSCP$DISK` client connect afterward (the joiner's
   143.891 half). Test the cheaper server-only path first — it may be sufficient.

The joiner-driven sequencer builders (`scs_connect_build_mscp_request`, op=3 confirm,
lookup-request) remain valid for the **formation** case and for OVMX's optional
reciprocal client half; they are not the established-join primary path.

### Byte-exact facts for the server-first implementation (from af2, verified)

- **`MSCP$DISK` lookup HIT result@92** = the **name echoed** space-padded:
  `4d534350244449534b20202020202020` (`"MSCP$DISK       "`). NOT the `VMS$VAXcluster`
  blob. `VMS$VAXcluster` HIT result@92 = `011b0103000000000000000000000600` — this
  **already matches** OVMX's `dir_affirmative_result` (scs_dir.c:113). So the fix is a
  **per-name** affirmative descriptor: `MSCP$DISK` → echo the 16-byte name; else the blob.
- **The member-initiated `MSCP$DISK` connect uses a DIFFERENT accept handshake than the
  directory connect.** Directory connect is `op0`(req)→`op1`(echo)→`op2`(response w/
  handle+names)→`op3`(confirm). The `MSCP$DISK` connect is:

  | t (s) | dir | mt | len | op | ss | ra | rc | lc | note |
  |-------|-----|----|-----|----|----|----|----|----|------|
  | 143.7579 | M→J | 4b | 110 | **0** | 7 | 5 | 0 | `3553000a` | member's connect-request, name=`MSCP$DISK` res=`VMS$DISK_CL_DRVR` |
  | 143.7582 | J→M | 4b | 66 | **1** | 7 | 7 | `3553000a` | 0 | joiner **echo** (no names) |
  | 143.7583 | J→M | 5b | **62** | **4** | 8 | 7 | `3553000a` | `8fd10008` | joiner **ACCEPT** — supplies OVMX's MSCP server handle (`lc`) |
  | 143.7585 | M→J | 4b | 58 | **5** | 9 | 8 | `8fd10008` | `3553000a` | member **confirm** |

  So OVMX-as-server must reply to the member's `op=0` MSCP connect with `op=1` echo
  (66-byte) then `op=4` accept (62-byte, binding OVMX's own MSCP server Con.ID as `lc`),
  and expect the member's `op=5` confirm. New builders needed (op=1 echo already exists
  as `scs_dir_build_connect_echo`? verify byte-shape; op=4 accept is NEW — a 62-byte SCA
  like the op=3 confirm but op field = 4 and it carries OVMX's server handle). Grab the
  exact op=4/op=1 templates from `af2-firsttimer-established-20260728.pcap` at t≈143.758.

**Implementation status:** grounded + byte-scoped, NOT yet coded. The live golden lab is
up (vax1+vax2, br0/tap1/tap2; build-d94 ready) so the server-only hypothesis (does
serving `MSCP$DISK` + accepting the member's MSCP connect + accepting the VC reach
MEMBER, without OVMX's own reciprocal client half?) is directly testable once coded.

## ►►► DEFINITIVE 2→3 REFERENCE — the joiner DRIVES (2026-07-30, `vax3-2to3-established-join-20260730.pcap`)

**We built a third real VAX and captured the exact scenario OVMX targets.** VAX3
(`SCSNODE=VAX3`, `SCSSYSTEMID=1027`, `VOTES=0`, root `[SYS2]`, MAC `08:00:2b:11:22:33`)
booted into the **live 2-node cluster** and became a MEMBER — `CLUSTER_NODES=3`. 17,705
frames. This supersedes af2 (a 1→2 formation) as the authority for an established join.

**THE MODEL WAS INVERTED. The joiner drives the ENTIRE connection set; the member only
answers.** Byte-verified sequence (VAX3 → VAX1, one shared send_seq that restarts at 1
after START):

| t (s) | dir | mt | ss | what |
|-------|-----|----|----|------|
| 28.884 | 3→1 | b3 | — | HELLO / channel |
| 28.886–28.889 | 3↔1 | 41 | 1 | **START** handshake (6 frames, incarnation=1) |
| *(~0.93 s gap)* | | | | |
| 29.8220 | **3→1** | **5b** | **1** | **joiner opens its OWN `SCS$DIRECTORY` connect (op=0)** |
| 29.8224/8227 | 1→3 | 5b | 1/2 | VAX1 op=1 echo + **op=2 ACCEPT** (handle `50be000c`) |
| 29.8229 | 3→1 | 5b | 2 | joiner op=3 confirm |
| 29.8229–8238 | 3→1 | 5b | 3,4,5 | lookups `MSCP$TAPE`, `MSCP$DISK`, `MSCP$DISK` |
| 29.8241 | **3→1** | **5b** | **6** | **joiner opens `MSCP$DISK` connect** → VAX1 op2 (`VMS$DISK_CL_DRVR`, `3552000d`) |
| 29.8243 | 3→1 | 4b | 7 | lookup `VMS$VAXcluster` |
| 29.8245 | 3→1 | 5b | 8 | op=3 confirm (MSCP) |
| 29.8247 | **3→1** | **5b** | **10** | **joiner opens `VMS$VAXcluster` VC** → VAX1 op2 (`3552000e`) |
| 29.8252 | 3→1 | 5b | 13 | op=3 confirm (VC) |
| 29.8253 | 3→1 | 4b | **14,15** | **joiner sends the 190-byte config burst on ITS VC** |
| 29.8256 | 1→3 | 4b | 14,15 | VAX1 answers config → reconfiguration → MEMBER |

### Why OVMX is refused

OVMX (pure-server) **waits** for VAX1 to drive, reaches NEW, and only *then* opens its
client dir connect — at shared seq ~13, long after VAX1 has built its own view. A real
joiner opens it as the **first sequenced message after START, at ss=1**. VAX1 accepts a
directory connect from a node that is *establishing itself*, not from one already parked
in NEW. That is the fundamental precondition the previous session could not ground.

Corollaries (all byte-verified here):
- **msgtype is 0x5b for the joiner's own connects** (VC not yet up), NOT the 0x4b the
  last session patched in from af2's late-phase frames.
- **Not stop-and-wait**: the joiner pipelines — it fires the `MSCP$DISK` connect (ss=6)
  and the `VMS$VAXcluster` lookup (ss=7) before earlier responses land.
- Both sides 0x48-credit-ack continuously throughout.
- The **VC the config burst rides is the JOINER's own** (`18e30009`↔`3552000e`), which
  is what `OVMX_JOIN_SEQ` was originally built to do.

**Next implementation step:** revive the joiner-driven path (`OVMX_JOIN_SEQ` lineage),
but with this ordering: START → *immediately* open own `SCS$DIRECTORY` (0x5b, ss=1) →
lookups → `MSCP$DISK` connect → `VMS$VAXcluster` connect → config burst on OUR VC, all
pipelined on one contiguous shared seq. The pure-server server-side responders stay (the
member still opens its own connections later); they simply are not the path to MEMBER.
