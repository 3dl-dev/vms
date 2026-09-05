# Research — the VMS lock manager's directory-node algorithm (FC-P4.1, IDSM-DIR)

> Answers the four FC-P4.1 questions from the published book: Roy G. Davis,
> *VAXcluster Principles* (Digital Press, 1993), chapters 6 and 7. Clean-room
> (Rule 8): page cites only; the host-only transcript is never committed and
> nothing is quoted beyond the book's own term names. Every statement below is
> either a paraphrase with a page cite, a measurement from our own captures
> and code, or labelled INFERRED. Written 2026-09-03.
>
> Verdict up front: **the vector and the index rule are fully published and
> are implemented from the book (rung A); the hash function is NOT published
> at the bit level — but the book documents that the 16-bit hash value
> travels on the wire in every directory lookup, so OVMX takes the value from
> the wire (rung A′) and never computes it.** Details, residuals and the exact
> P4.3 contract follow.

## 1. Vector construction — the Lock Directory Weight Vector (FULLY GROUNDED)

| Fact | Page |
|---|---|
| A root resource's directory node is found by dividing the resource name's hash value by the number of entries in the Lock Directory Weight Vector; the remainder indexes the vector; the entry is the directory node's CSID. | 6-31 |
| Only **root** resources are looked up. A tree is mastered where its root is; the master's CSID is saved with the root and propagated down every branch, so a system performs exactly one directory lookup per tree while it holds any lock in it. | 6-31, 6-32 |
| The three outcomes of a lookup sent to the directory node: (1) the directory node is also the master and resolves the request; (2) it knows the master and returns the master's CSID, and the requester re-sends to the master; (3) it knows no master, tells the requester to **become** the master, and records that. If the requester *is* the directory node, absence of the root in its own database means nobody masters it and it assumes mastery. | 6-31 |
| **Entries per system = that system's LOCKDIRWT.** If LOCKDIRWT is 0 on *every* member, the vector is forced to hold **one entry per system**. All entries for one system are **contiguous**. | 6-32 |
| Every member's copy has the same number of entries and the same system at each offset ("logically equivalent"); the only difference is that a system's **own** entries read **0** in its own copy and its CSID in every other copy. | 6-32, Fig. 6-18 p. 6-33 |
| Worked example: VAX_A weight 1, VAX_B weight 3, others 0 → a 4-entry vector [A, B, B, B]; VAX_B is directory for ≈3× as many trees as VAX_A; the 0-weight systems are directory for none. | 6-32, 6-33 |
| The vector is initialized when the cluster forms; a joining system with nonzero weight **adds** entries; a departing nonzero-weight system's entries are **removed** and the copies collapse; the new system is given what it needs to build an equivalent copy. Because these changes can move a name to a different directory node, **all directory information cluster-wide is discarded** during such transitions and masters re-register with the (new) directory nodes — the **directory rebuild**. | 6-33 |
| Rebuild type when a system joins: joiner weight > 0 → directory rebuild; all selected systems weight 0 → directory rebuild (every system must have one entry, so the joiner takes its share); joiner weight 0 with at least one nonzero elsewhere → **merge** rebuild. | 7-40 |
| Rebuild type when a system leaves: a nonzero-weight system leaving, or all remaining systems at weight 0 → directory rebuild; otherwise partial rebuild (the V5.2+ table: 0-weight join → merge, 0-weight leave → partial, nonzero join → directory+merge, nonzero leave → directory+partial). | 7-43, 6-36 |
| **Timing.** Phase 1: the coordinator picks the rebuild type and **adjusts the size** of its vector (grows it by the joiner's weight if nonzero; nothing if 0), sends the Phase 1 message; each recipient stores the proposed cells and the rebuild type and adjusts (the joiner *allocates*) its own vector. Phase 2 (committed): after MEMBER flags, the CSV entry, and the CLUSTER flag, **the vector is filled in to reflect the current membership**, then all systems wait for each other and perform the rebuild **in unison, synchronized by the coordinator** — the transition ends when that rebuild ends. | 7-40, 7-41, 7-42 (join); 7-36, 7-37 (form); 7-46, 7-47 (leave) |
| Merge rebuild = a 0-weight joiner registers, with the directory nodes, the trees it already masters (its boot-time node-specific system locks) by running the directory algorithm for each. | 6-35 |

**Member ORDER within the vector — NOT stated.** The book fixes that each
system's entries are contiguous and that offsets agree cluster-wide, but not
the order in which systems are laid out. The natural candidate is Cluster
System Vector index order (the low 16 bits of the CSID index the CSV, p. 7-39
n.†, 7-39/7-40 on CSV entries); the figure on p. 6-33 lists VAX_A before
VAX_B. **This residual is self-checking from received traffic** (§4 below):
every lookup OVMX receives as a directory node carries the sender's hash; if
`hash mod n` does not land on one of OVMX's own entries under the assumed
order, the order hypothesis is falsified with no capture needed.

Two consequences for OVMX that the coordinator's prior reading got right:

- With every member at LOCKDIRWT=0 — the lab's likely configuration (P3.2
  verifies) — **OVMX is a directory node for ≈1/N of all root names** the
  moment it joins, and its join triggers a **directory rebuild** (p. 7-40).
  The 222 op-0d records VAX1 pushed at OVMX during the join are, on this
  reading, masters re-registering trees with their new directory node
  (p. 6-33) — INFERRED; the vax3 capture (FC-P5.1) confirms.
- A directory node stores, per registered root, an RSB flagged as a
  *directory entry* carrying the master's CSID (p. 6-50); it is not the master
  and holds no lock. That is the "directory-node duty ≠ mastering"
  distinction, now on the book's authority.

## 2. Hash input (PARTIALLY GROUNDED)

| Fact | Page |
|---|---|
| The hash is computed over **the bytes of the resource name character string**, producing a **16-bit** value. | 6-49 |
| The same algorithm on every system yields the same value for the same name — the consistency requirement. | 6-32 |
| The 16-bit value is also what indexes the local Resource Hash Table (right-shifted by 16 − n bits where 2ⁿ ≥ RESHASHTBL). | 6-49 n.†, 6-50 |
| Directory lookups are on the **root** resource name. | 6-31 |

**Not stated:** whether the name length, padding to 31 bytes, the access
mode, or the parent's identity participate (for a *root* the parent is
null); whether the string is normalized. Because the value is on the wire
(§3), none of this needs to be settled to route correctly.

## 3. Is the hash FUNCTION given bit-level? — NO. But the VALUE is on the wire.

The book describes the function only abstractly: a series of logical and
arithmetic operations on the name's bytes producing a 16-bit result
(p. 6-49). No fold, shift schedule, CRC, or polynomial is given anywhere in
ch. 6 (checked pp. 6-18 – 6-53). **Rung A for the function itself is
therefore unavailable, and deriving it from observation is Rule-8-forbidden
absent an operator ruling.**

The decisive published fact for the design is on **p. 6-50**: a directory
lookup request carries the root resource name **and the hash value derived
from it by the sending system**, "as an optimization" because every system
would derive the same value; the directory node right-shifts the received
value to index its own Resource Hash Table and scans that chain for the
name. So:

- the 16-bit hash value is a **wire field of the lookup request**, sent by
  VMS itself for every root name any node looks up;
- a directory node is *expected to use the received value*, not recompute;
- for any name OVMX has ever seen looked up (as directory node, as master
  receiving requests, or in rebuild registrations), OVMX **knows the hash
  value the cluster uses** — read off the wire, never computed.

**Which byte is it?** The strawman's op-01 builder placed a 16-bit
`dir_hash` at **body[10:12]** and sent it as "honest 0"
(`feat/coord-rebuild-completion:src/vmsscs/scs_member.c:852`), and VAX1
accepted those 48 registrations. That the field *is* the hash value is
INFERRED from the builder's naming; it is confirmed offline from existing
captures (no lab time) by two properties every hash field must have:
(a) for one resource name, body[10:12] is identical across all senders and
all occurrences; (b) it differs across names. FC-P4.2 adds this check to
its offline half (§5).

**A corollary that explains the campaign's "grant storm" (INFERRED, but it
fits the book exactly).** A lookup carrying hash 0 makes the directory node
index chain 0 and scan it for the name; the name's real RSB lives on
another chain, so it is not found and the directory node takes outcome (3):
it creates a directory entry naming the *requester* as master (p. 6-31,
6-50). That is what "VAX1 GRANTED all 48" most plausibly was — VAX1 telling
OVMX "you master it" — after which VAX1's own lock requests for
`F11B$aSYSDSK1`/`LNM$CWLOGICALS` went to OVMX as master, OVMX answered
them as if they were grants, and VAX1 re-requested forever (memory
`cluster-promotion-gap`, the 35/s storm). Sending a placeholder hash is not
"honest 0"; it silently corrupts the peer's directory. **INV-6 rule for
P4.3: a lookup is sent only with a hash value received from the wire for
that exact name; a name with no known hash is never looked up with a
guess.**

## 4. Recommendation — rung A for the vector and index rule, rung A′ (hash-from-the-wire) for the value; probe/ruling only for the residual

**Build now (rung A, fully grounded):**

1. `struct vms_ldwv` in the CLUB: `n` entries of CSID (own entries = 0),
   built at Phase 2 fill from the member set and each member's LOCKDIRWT
   (P3.2 pins the wire field; until then the member set with all weights 0
   is the honest reading and yields one entry per member). Contiguous runs
   per system in CSV-index order (the order hypothesis, self-checked as
   below). Resized at Phase 1 per the coordinator's rule; refilled at Phase
   2; all `rsb->dir_csid` invalidated on any transition that changes the
   vector (p. 6-33).
2. `dir_resolve(rsb)`: `idx = rsb->hash16 mod n`; `csid = ldwv[idx]`; a 0
   entry means *this node* is the directory (p. 6-32); requires
   `rsb->hash_known`.
3. Directory-node role (P5.4): on a received lookup, take `hash16` from the
   request, index the local resource table with it, apply outcomes (1)–(3),
   flag the RSB as a directory entry with the master's CSID (p. 6-50).
4. Self-check: every received lookup must satisfy
   `ldwv[hash16 mod n] == 0` (i.e. addressed to us). A miss is counted
   (`dir_lookup_misaddressed`) and logged with the sender's CSID; a sustained
   miss rate falsifies the order hypothesis (or a stale vector) and is the
   alarm, never silently served.
5. `rsb->hash16` learned from **every** cat-02 frame that carries the field
   for a name — lookups received, requests received as master, rebuild
   registrations — stored with `hash_known = 1`; `exec_jhash` deleted.

**Requester routing (rung A′):** a `$ENQ` on a root whose `hash_known` is
set routes by rule 2 — correct by the book and never computed. This covers
every cluster-shared name OVMX will meet in practice (system, volume and
file locks all appear on the wire before OVMX contends on them; the
rebuild registrations alone deliver hundreds).

**The residual: a root name OVMX is the first in the cluster to touch**
(its own volume label `F11B$v<label>`, `RMS$` locks on its own files).
Its hash is unknown and cannot be computed. Options, in order:

- (a) If OVMX is a directory node and the name would index OVMX's own
  entries, it may assume mastery locally (p. 6-31) — but it cannot know
  that without the hash. Not usable.
- (b) **Probe** the weighted set (design §3.6 rung B) — safe only if a
  mis-addressed lookup is refused rather than served; **the book gives a
  reason to expect it is served** (outcome (3) creates an entry without any
  "am I the directory?" check being described, p. 6-31). FC-P4.2's
  mis-addressing test decides; the expectation is *unsafe*.
- (c) Operator ruling (design §3.6 rung C(i)): treat the hash as an
  interop-necessary transform whose input/output pairs VMS itself
  broadcasts in the clear on every lookup, and permit black-box
  determination from observed pairs (never from binaries). The framing for
  the operator: the protocol *requires* a conforming node to produce this
  value, and the reference implementation *publishes* its values on the
  wire by design (p. 6-50). This is the classic protected-interoperability
  case, but it is a change to Rule 8's stated scope, so it is theirs.
- (d) Until (c) is ruled or (b) proves safe: a `$ENQ` on a novel root name
  returns `SS$_UNSUPPORTED` with one `%CNXMAN` line — **only for names
  never seen on the wire**, which for a joining OVMX means its own private
  volumes/files. Membership, directory-node duty, mastering of assigned
  trees, and locking on every shared name are unaffected. This is the
  honest floor, and it is narrow.

**Recommendation: proceed with rung A + A′ immediately (FC-P4.3 is
buildable); run FC-P4.2's two checks; put (c) to the operator with the
p. 6-50 framing in parallel.** The DLM-rebuild chain (P4.6, P5.3/5.4/5.5)
does not wait on the residual — the rebuild is driven by received
registrations, which all carry the hash.

## 5. What FC-P4.2 measures (revised)

Offline, from existing captures (no lab time): (1) confirm body[10:12] is
the hash — constant per name across senders/occurrences, varying across
names — over `/tmp/ref.pcap` and the db20-b/2f2025a7 pcaps; (2) with the
lab's LOCKDIRWT values (P3.2) and the CSV-order hypothesis, check that
every lookup's destination equals `ldwv[hash mod n]` — zero residuals =
the order is confirmed; a systematic permutation = the order is something
else, and the same data identifies it (it is a permutation of ≤ 3
systems).

On the clone (one session): the mis-addressed-lookup test (design §5.2(2)b)
— send one lookup for a name whose directory is VAX2 to VAX1 with the
*correct* hash and read VAX1's reply class and its `SDA> SHOW RESOURCE`
afterward. Entry created ⇒ probing is unsafe ⇒ residual goes to (c)/(d).

## 6. Traceability

- Design: `docs/design-faithful-cluster-executive.md` §3.6 D-DLM-2 (rung
  ladder, updated to A / A′ / B / C by this note), §5.2.
- Plan: FC-P4.1 (this note), FC-P4.2 (checks above), FC-P4.3 (`dir_resolve`
  + LDWV + hash-from-the-wire + self-check), FC-P5.4 (directory-node role
  uses the received hash).
- Book: Davis, *VAXcluster Principles*, pp. 6-31–6-36, 6-49–6-50, 7-36–7-47
  (host-only transcript `~/cluster/transcript/ch6-part02.md`,
  `ch6-part03.md`, `ch7-part03.md`; page cites only).
