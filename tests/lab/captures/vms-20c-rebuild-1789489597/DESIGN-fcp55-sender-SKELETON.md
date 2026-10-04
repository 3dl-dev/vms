# FC-P5.5 design SKELETON — the survivor-side cat-0x02 op-0x0d REBUILD sender (vms-20c)

Status: SKELETON for the ⭐⭐ conductor review. Mechanism + executive-read contract are
authored from the GROUNDED capture (this directory). The exact op-0x0d field map and the
code-integration points are PLACEHOLDERS to fill from (a) the conductor's grounded op-0x0d
record layout and (b) origin/main code (this author's local checkout is pinned-stale, so no
code-line claims are made here without that confirmation).

## 0. Grounding provenance (why this design is not LARP)
The 2026-09-15 own-lab capture (`rejoin-rebuild.pcap` + `sda-before-vax1.txt`) proved, by
the conductor's ⭐⭐ decode correlated to SDA:
- On a transition, the survivor emits **307 cat-0x02 op-0x0d REBUILD frames** carrying its
  held-lock records to the rejoining/new-master node (VAX1→VAX2). The 3 cat-0x01 op-0x05
  frames are the phase SIGNAL, not the record carrier.
- op-0x0d bodies carry REAL records — resnam@b[48], namelen@b[47] — decoding to canonical
  VMS cluster locks (SYS$SYS_ID len16, F11B$a/bSYSDSK1, MSCP$LOADBALSK1, VCC$vSYSDSK1),
  each matching a real VAX1 SDA lock (id/mode/len).

So the record format is the ALREADY-GROUNDED cat-0x02 op-0x0d DLM family; the consume side
already exists (vms_lock_dlm_xnode_rebuild, vms_lock.c:2442 — [confirm vs origin/main]).

## 1. The gap (what this design builds)
NOT a new op-0x05 message class. The single missing piece: the **survivor-side SENDER** that,
on a cluster transition, emits cat-0x02 op-0x0d REBUILD records for the locks it holds, from
LIVE executive state, to the node that becomes the resource's new master. Its receiver
(consume/reconstruct) and the op-0x0d parse codec already exist.

## 2. Mechanism (executive-resident; never a userspace daemon)
Trigger — DEFINITIVE (conductor, origin/main): `vms_ioctl_dlm_member_depart`
(vms_lock.c:~1640, the H10a departure hook scsd calls on SCS_MEMBER_OP_DEPART) already does
the remaster: it invalidates res->dir_csid/master_csid and rehashes, and carries the verbatim
TODO that IS FC-P5.5 — *"A lock's STATE is NOT reconstructed here -- that (collecting
survivors' origin records + rebuilding res->granted) is the H10b rung (vms-dca9)."*
- NEW-MASTER-per-resource = `dlm_directory_csid(resnam) % members_live` (the rehash over the
  shrunk live set; vms_lock.c:410,1645).
- INSERTION POINT: right after that rehash — for each held lock on an affected resource,
  send an op-0x0d record to that resource's new master. Same for the ADD/FORM transition
  equivalent (the join-side rebuild).

On that trigger, for the set of locks this node holds whose master is affected by the
transition (departed master → new master, or FORM/ADD redistribution):

  for each affected held lock L:
      read LIVE from the executive lock DB / res->granted (NOT templated, NOT frame-echoed):
         resnam, namelen, lkmode (granted mode), req_lkid, req_csid, master_lkid
      build a cat-0x02 op-0x0d REBUILD record via the grounded layout (§3)
      dlm_arm_send it to the resource's NEW master

## 3. op-0x0d REBUILD record layout (⭐⭐ — FILL FROM CONDUCTOR'S GROUNDED MAP)
Body-relative (body[0] = frame abs 72; note b[48] = frame abs 120 = the DLM-family resource
position). GROUNDED so far (conductor's decode, 304/307 correlated to sda-before-vax1):
- b[8:10]  = 02 0d (cat/op).
- b[48]    = RESOURCE — a **binary VMS resource descriptor** (16–31 bytes: ASCII-ish name +
  binary qualifier bytes, e.g. `SYS$SYS_ID` carries a trailing `01 04`). Treat as the
  fixed-position DLM-family resource field, NOT a clean namelen-prefixed C-string.
- b[47]    ≈ resource length indicator (SYS$SYS_ID → 16, == SDA "Length 16").
- b[16:48] region carries the lock fields (mode / req_lkid / req_csid / master_* / valblk) —
  EXACT OFFSETS pinned by the codec TWIN TEST against the capture (§3a), NOT assumed.

⚠ EMPIRICAL FINDING (this author, cross-ref of the capture): op-0x0d does **NOT** reuse the
grounded ENQ field offsets. Over 203 resnames shared between op-01 ENQ and op-0x0d frames:
req_lkid@data[92] matched 0/203, master@data[96] 0/203 (mode@data[102] 53/203, i.e. chance for
a small-value byte). op-0d @data[92] holds small ints (record/member counts) and @data[96]
holds mixed/name-like bytes (e.g. "DIRE"). So the op-0x0d record is its OWN layout — the only
grounded anchors are cat/op@b[8:10] and resource@b[48]/len~b[47]; the lock fields' offsets are
UNKNOWN until the twin test pins them. Do not port the ENQ offsets.

### 3a. HOW the offsets get pinned (the ⭐⭐ grounding, non-fabricable)
Build the op-0x0d record codec with a TWIN TEST whose fixtures ARE the captured frames
(rejoin-rebuild.pcap = 307 real op-0x0d) + the SDA oracle (sda-before-vax1.txt, 148 locks):
decode a real op-0x0d body → re-encode → assert BYTE-IDENTICAL, AND assert each decoded
{resnam, req_lkid, mode} correlates to its matched SDA lock. A wrong offset FAILS the
round-trip or the SDA correlation → the test is red, never a silent fabrication. Offsets are
derived by cross-ref (a resname shared with an op-01 ENQ frame → the ENQ's grounded req_lkid
value → find where that value sits in the op-0x0d body → pins the op-0x0d offset), then LOCKED
by the twin test's byte-identical + SDA assertions.

⚠ FACADE TO KILL (conductor, origin/main): do NOT reuse `scs_dlm_build_body(SCS_DLM_OP_REBUILD=5)`
— it writes wire op byte **5**, an OVMX-INVENTED H10b encoding, NOT the real cat-0x02 op-0x0d
the capture grounded. The RESPONSE/echo path (scs_member_build_dlm_response) and the CONSUME
(vms_lock.c:2442) exist; the record SENDER does not — build it to the REAL captured op-0x0d,
and let the twin-test-against-capture expose and kill any op-5-vs-op-0x0d divergence. That
divergence is exactly the facade the capture was worth catching.
Each field is annotated with the executive accessor it is read from at send time. NOTE the
wire `req_lkid` is the REQUESTER-SIDE handle (≠ SDA's local "Lock id" field — that's why a
naive SDA-lockid search only matched SYS$SYS_ID; the offset is pinned by the op-01 cross-ref).
| field       | wire offset            | executive source (read live at send)        |
|-------------|------------------------|---------------------------------------------|
| resource    | b[48] (abs120), len ~b[47] | res->name (+ the VMS resource qualifier)  |
| lkmode      | b[16:48] [from op-01 xref] | lkb granted mode (res->granted)          |
| req_lkid    | b[16:48] [from op-01 xref] | the holder's REQUESTER-side lock handle   |
| req_csid    | b[16:48] [from op-01 xref] | this node's CSID (membership)             |
| master_lkid | b[16:48] [from op-01 xref] | the master's lock id for this resource    |
| valblk      | b[16:48] [from op-01 xref] | res value block (when LCK$M_VALBLK)        |

## 4. INV-6 / never-crash discipline (the ⭐⭐ bar)
- Every emitted field is a LIVE read at send time; a lock that has been DEQ'd / is transient
  is OMITTED, never emitted as a zero/placeholder record (no fabricated referent).
- The sender never crashes a peer: bounded record count, exact-sized frames, and it emits
  nothing for a resource whose master it cannot determine (honest omission).
- Determinism: the record set is exactly this node's real granted locks for affected
  resources — verifiable against SDA SHOW LOCKS (the capture's correlation method).

### 4a. ⚠⚠ CRASH-PREVENTION GATE (real precedent — this is why byte-exact is mandatory)
The op-0x0d codec on main deliberately treats the record as an OPAQUE VERBATIM ECHO
(struct = uint8_t body[132] verbatim + a convenience name), because field-by-field
reconstruction of this frame MIS-SHIFTED the resource name and **BUGCHECKED TWO REAL VAXES
with LOCKMGRERR** (specimen `ovmx-760-lockmgrerr-20260730.pcap`). A receiving master that
mis-reads a shifted record bugchecks — a crashed peer, the exact ⭐⭐ failure this gate exists
to prevent.
THE LANDMINE FOR THIS SENDER: unlike the echo path, the sender has NO source frame — it must
FIELD-ASSEMBLE the op-0x0d record for its own held locks. That is the precise operation that
crashed two VAXes. Therefore:
1. **BYTE-IDENTICAL, not "correct fields."** The sender's record builder MUST be twin-tested
   to produce output BYTE-IDENTICAL to the real captured op-0x0d frames. Byte-identical is the
   CRASH-PREVENTION gate, not mere rigor: any shift = LOCKMGRERR on the receiving master.
2. **STALE-BUFFER regions:** VMS emits fixed-size frames with prior-frame leftover in some
   regions (e.g. body[36:52]); the codec uses EXACT-MATCH MARKERS to avoid mis-reading them.
   The builder must NOT invent zeros/garbage where VMS leaves stale buffer — match VMS's actual
   pattern and honor the marker discipline so a built record never trips the receiver's marker
   mis-read. A region is treated as don't-care ONLY if the capture proves it marker-keyed — never
   assumed.
3. o5's never-crash-a-peer FUZZ on the sender is MANDATORY (in the split) — but the PRIMARY gate
   is byte-identical-to-capture; fuzz is the secondary net.

### 4b. HARD SEQUENCING (crash guard)
The conductor's codec twin-test — byte-identical to the 307 captured op-0x0d frames — MUST land
and be GREEN before this sender emits a single record. The sender links against that byte-exact
codec; it is UNSAFE to emit until the codec is proven byte-exact. Codec-first is not a
preference, it is the crash guard.

## 5. Component split (per the conductor-agreed division)
- THIS AUTHOR (grounding-sensitive): the op-0x0d record BUILDER (from real state) + the
  CNXMAN transition wiring + the executive field-reads (§2–§4).
- cluster-impl-o5 (from this reviewed design): rebuild-type SELECTION per class
  (merge/directory/partial/full per LOCKDIRWT + FORM-vs-transition), 3-tree concurrency +
  fault-tolerance, the never-crash-a-peer fuzz on the sender, the #928 NetBSD mirror, and
  the multi-node proof rig.

## 6. Proof
Re-run the own-lab rejoin (this capture's method) with OVMX as the survivor: assert OVMX
emits op-0x0d records byte-shaped like the real-VAX oracle for the SAME held locks (SDA
correlation), and the peer consumes them without crash. Departure survivor→new-master
(3-node) is the ci.6 end-to-end proof.

## OPEN / RESOLVED
1. Exact op-0x0d field offsets/widths in b[16:48] — RESOLVED-BY-METHOD: pinned by the codec
   twin test against the capture (§3a). Not the ENQ offsets (empirical finding, §3). This is
   the one remaining derivation, and it is self-validating.
2. Does a cat-0x02 op-0x0d REQUEST/record builder exist? — RESOLVED (conductor/origin/main):
   NO. Response/echo (scs_member_build_dlm_response) + consume (vms_lock.c:2442) exist; the
   record SENDER does not. ⚠ Do NOT reuse the op-byte-5 H10b encoder (§3 facade warning).
3. CNXMAN transition hook + new-master-per-resource — RESOLVED (conductor/origin/main):
   insert after the rehash in `vms_ioctl_dlm_member_depart` (vms_lock.c:~1640) at the
   "STATE is NOT reconstructed here" TODO; new master = `dlm_directory_csid(resnam) %
   members_live` (§2).

## RE-GROUNDING CORRECTIONS (origin/main, conductor 2026-09-15)
⚠ Earlier code line refs came from a stale checkout (work/vms-cd3, 234 behind main). Corrected
against origin/main below — CONFIRM these when in a clean main worktree (this author's tree is
also stale, so these are the conductor's origin/main reads, not independently verified here):
- CONSUME `vms_lock_dlm_xnode_rebuild` = vms_lock.c:**4138** (was 2442).
- INSERTION `vms_ioctl_dlm_member_depart` = vms_lock.c:**3445** (was ~1640); FC-P4.3-reworked on
  main — a resource mastered on the departed node re-masters on first use; the LOCK STATE the
  departure orphans is "a real lock the survivors must describe to the new master" (vms_lock.c:3342).
  Insertion CONCEPT holds; the surrounding structure is new — re-read before hooking.
- CODEC ALREADY PARTIALLY EXISTS on main (scratch "build from scratch" AND "op-5 facade"):
  `vms_cluster_codec_dlm.c` has the op-0x0d codec (`vms_dlm_rebuild_parse_body` +
  `vms_dlm_rebuild_response_build`, WIREOP_REBUILD=0x0d, registry cat0x02/op0x0d §4(p),
  1367/1367 responses grounded). FC-P4.6 already renamed the op-5→0x0d encoding, so there is NO
  op-5 facade on main. BUT the parse captures the record as an OPAQUE ECHO BLOB (get_bytes
  0..ECHO_LEN, to bounce back) — it does NOT parse the record's internal {resnam,lkid,csid,mode}.
  So the CODEC GAP is ADDITIVE: add STRUCTURED field parse/build of the op-0x0d record (grounded
  to the decoded layout via the twin-test) on top of the existing echo codec. (Conductor owns
  this, on a clean main worktree.)
- SENDER/BUILDER genuinely absent = vms_dlm_scs.c:**4307** ("still absent") — the real gap (mine).
- ⚠ THE CAPTURE CORRECTS MAIN'S OWN STALE PREMISE: coord_fsm.c:1222 still says "the op-0x05
  lock/resource rebuild burst still has NO builder -- honestly absent." The wire capture proves
  the RECORDS ride op-0x0d (307 frames); op-0x05 is only the 3-frame phase signal. The code's
  premise (op-0x05 carries the records) is WRONG — the sender builds op-0x0d records, and that
  comment must be corrected when the sender lands.

## Status: design phase essentially closed
Skeleton ⭐⭐-APPROVED by the conductor. Revised NET scope: (1) CODEC (conductor, clean main
worktree) — add structured op-0x0d field parse/build to the existing echo codec, twin-tested
against the 307 captures; (2) SENDER (this lane, fresh env) — build op-0x0d records from held
locks + send at the vms_lock.c:3445 member_depart insertion, correcting the op-0x05 premise;
(3) o5 — the sub-FSMs (§5). Grounding is DONE and premise-corrected (records ride cat-0x02
op-0x0d, not a new op-0x05 class). The wire handle is a REMOTE/master-side reference (grounds
structurally via byte-identical round-trip, not VAX1-SDA correlation); b[24]=parent region
(groups-of-52, parent-present flag); b[36]/b[40]=0 constants.
