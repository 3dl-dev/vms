# Design: Executive cluster GENESIS — OVMX as a first-class founding member (vms-3c3)

**Status:** APPROVED (Baron, 2026-09-10) — quorum-grounded gen-1 mint is the
sanctioned mechanism. Prerequisite for vms-1ee (real 2-node DLM wire proof).
**Date:** 2026-09-10. **Lane:** cluster.

## Framing: this is clustering ROBUSTNESS, not a separate platform

The goal is **not** "OVMX-only clustering" (a dangerous framing — it implies a
divergent OVMX-OVMX platform). The goal is **clustering robustness**: OVMX
becomes a first-class VMScluster participant that can also be a **founding
member**, via the *authentic* VMS connection-manager formation algorithm. The
outcome we want is that **OVMX^n AND mixed (OVMX)^n·(OpenVMS)^m** clusters all
work. Genesis is "OVMX can legitimately be the first member of a normal
VMScluster," using the same mechanism a real VAX uses — not a new kind of cluster.

**Two footguns this design actively prevents (hard requirements):**
1. **No regression to OVMX↔real-VAX interop** (the CN=3 join). With no
   genesis-eligible config the behavior is byte-identical to today.
2. **No divergent OVMX-only code path.** `cnxman_try_genesis` reuses the SAME
   `learn_local_csid` + coordinator + phase2 machinery the interop join uses.
   And it **never founds a competing cluster** beside a present coordinator: it
   fires only after `cnxman_join_drive()` finds no one to join, respecting VMS's
   "waiting to form or join an OpenVMS Cluster" wait window — so an
   eligible-by-votes OVMX node **joins** a present VAX (or OVMX) coordinator
   rather than founding a rival singleton. (VAXCLUSTER=1 "when present" never
   founds from cold; VAXCLUSTER=2 "always" may found only after the wait window
   with quorum met by its own votes.)

## The problem: a from-nothing cluster has no founder today

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
`connection-manager.yaml` makes no `verified` OVMX-founded-cluster claim
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

## The operator gate — RESOLVED (Baron, 2026-09-10)

Genesis is a **new externally-visible capability** (OVMX can be the founding
member of a VMScluster) and its mint touches the authenticity posture that #1052
tightened, so it was gated. Baron's ruling:

1. **Quorum-grounded gen-1 founding is the SANCTIONED mechanism.** It is the
   documented VMS formation algorithm and INV-6-clean by the votes gate above.
2. **Scope: this is a 1.0 HEADLINE** — clustering robustness (OVMX^n and mixed
   OVMX+OpenVMS), not accept-`implemented`. Build genesis now, then the 2-node
   DLM proof on top.
3. **Generation source.** Gen 1 for a from-nothing founder is unambiguous. A
   *re-formation* after total cluster loss should advance the generation; for
   1.0's lab use, gen-1-from-cold is sufficient. Flag if reformation generation
   must be persisted (tracked, not blocking).

## Implementation + test plan

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
- **Interop-safety (footgun guards — the two Baron flagged):**
  1. *No competing-cluster:* assert that when a coordinator (VAX or OVMX) is
     discoverable, an eligible-by-votes node **joins** it and does NOT found a
     rival singleton — i.e. `cnxman_try_genesis` fires only after
     `cnxman_join_drive()` finds nothing, and honors the VAXCLUSTER wait window
     (=1 never founds from cold; =2 founds only after the wait with own-vote
     quorum).
  2. *No interop regression:* the OVMX↔real-VAX join path is byte-unperturbed
     with no genesis-eligible config (the CN=3 join behaves identically). Reuse
     the existing single code path — genesis is a shared-mechanism addition, not
     an OVMX-only branch — so this is an assertion + the existing cluster
     interop/negctl gates staying green, not a new subsystem.
- **The 2-node proof (vms-1ee):** hermetic QEMU `socket` mcast rig
  (run_dlm_h1_derisk.sh pattern) on the k3s-worker KVM pod (worker exposes
  /dev/kvm+vmx; workshop is OOM-barred by operator directive). Node A: VOTES=1,
  EXPECTED_VOTES=1 → founds gen-1. Node B: VOTES=0 → joins A. Then drive the DLM
  rungs (ENQ/GRANT/BLKAST, H8/H9 LVB crossing, H10a/b remaster, e84
  dir-ownership, ec75 deadlock) with every result read from **real executive
  state on both nodes** (INV-6) — restoring the retired `verified`.

## As built (implementation deltas, operator correction 2026-09-10)

The mechanism above is what shipped. Two clauses of the predicate were
**tightened** on the operator's clustering-robustness correction — genesis is
"OVMX can legitimately be the FIRST member of a normal VMScluster", never a
second, OVMX-only kind of cluster, so nothing may risk a singleton forming
beside a live coordinator (a real VAX included):

1. **VAXCLUSTER = 2 only** (the note above said `>= 1`). "Always a member" is
   the configuration that asks for a cluster whether or not one is present, so
   forming one honours it. VAXCLUSTER = 1 is "a member only when a cluster is
   PRESENT" — from cold there is none present, so a =1 node stays STANDALONE
   and joins one when it appears. Strictly fewer founds than the note's rule.

2. **A discovery window before the first attempt.** Founding is NOT attempted
   at CLUSTER_START: the port has only just come up, so an empty CSB table then
   means "nobody has been heard yet", not "nobody is there". `CLUSTER_START`
   arms a window of **RECNXINTERVAL** — the executive's own configured answer
   to "how long before a system's absence is real", not a new timer — and the
   once-a-second reconnect beat (which is where discovery already runs) makes
   the attempt when it elapses, still finding nobody. The beat also re-tries
   forever, so a node that could not found at first (a peer came and went) can
   still form later, which is what "waiting to form or join" means.

Two further guarantees are enforced **inside** `cnxman_coord_found()` rather
than only in the caller's ordering, so they cannot be lost to a future call
site: any non-local CSB with a known SCSSYSTEMID (discovered OR admitted) makes
this node JOIN rather than form (`genesis_refused_peer`), and a CSID whose CSV
slot the grounded nodemap byte cannot name is refused rather than minted.

There is **no founding-only membership path**: `cnxman_coord_found()` mints
through `cnxman_club_learn_local_csid()` (the same and only setter the op-0x06
learn path uses), opens an ordinary class-ADD transition through
`coord_open_transition()`, and commits through `cnxman_phase2_commit()` — the
same functions an admission from a real VAX runs. With no participants the
transition emits **zero frames** (the existing degenerate 12 x (M-1) = 0 path).

Tests: `tests/cluster/host/test_cnxman_genesis.c` (predicate truth table +
end-to-end founding) and `tests/cluster/host/test_cnxman_genesis_negctl.c`
(every refusal, each with the CLUB compared byte-for-byte before and after).

## Update 2026-09-23 (vms-151): SYMMETRIC genesis — two fresh nodes that can see each other

The mechanism above forms a cluster when a node finds **nobody**. It could not
form one when two fresh nodes found **each other**, which is the ordinary way an
all-OVMX cluster comes up. Measured: two OVMX/x86 nodes (OVMXA/1987,
OVMXD/1990, group 257) booted in the browser, their SCS virtual circuits opened
bidirectionally in ~15 s and stayed open — and 25 minutes later both consoles
still showed a blank CSID and neither had ever printed `%CNXMAN, this node is
now a VAXcluster member`. Each held a CSB for the other, so
`coord_has_peer_csb()` refused **both**; and neither could be joined, because
admission needs a coordinator and a coordinator needs a CSID.

### Root cause 1 — the gate conflated three different facts

"There is a system present" was the right question. "Therefore I join it" was
the wrong conclusion: a system that is not in a cluster cannot admit anybody.
`cnxman_coord_found()` now asks the three separately, each one a read of real
CSB state (`vms_cnxman_coord_fsm.h` §8b, "THE ELECTION"):

1. **That system already holds a cluster identity** — a CSID on its CSB, or
   this CLUB's own MEMBER/SELECTED flag → **JOIN, never form**. This is the
   clause that keeps a booting OVMX node from forming a singleton beside a live
   VAXcluster, and it is *unchanged in strength* (footgun #1 above).
   `genesis_refused_peer`.
2. **That system has not been asked yet** → ask before forming. What settles
   "is there a cluster here?" is the question the join FSM already asks: a
   member takes a membership request and coordinates an admission within
   milliseconds (spec §4(o)); a system that is not in a cluster cannot. So a
   node may form only after at least one **complete** round in which every
   system it could see was asked and none took the request — the join FSM's own
   `attempts_exhausted`, handed in as `struct cnxman_form_evidence`, never
   inferred by the coordinator and never cached by the glue.
   `genesis_refused_unasked`.
3. **That system is another founding candidate** → exactly one of us forms.
   Book p. 7-32's published mechanism is the **coordinator lock** (ask every
   selected system, one already granted refuses, collisions back off a random
   short interval). OVMX **cannot ask**: no capture in the library contains a
   coordinator-lock request or grant and no opcode is grounded for one, and
   inventing a frame for it is the failure class that bugchecked two real VAXes.
   What ships instead is a **total order over a value every candidate already
   advertises** — the SCSSYSTEMID in its CSB, **lowest first**. It is labelled
   for what it is: an **OVMX design value** standing in for a mechanism OVMX has
   no grounding to speak. The properties that matter are that it is total (it
   cannot elect two), symmetric (both nodes decide identically from the same
   wire-learned numbers) and needs no frame; the *direction* is arbitrary and is
   not claimed to be VMS's. Who counts as a candidate is read too: FORM requires
   VOTES > 0 (pp. 7-28, 7-33), so a peer whose own PARAMS advertise zero votes
   can never form and is not a rival (deferring to it would be the same deadlock
   in a new shape), while a peer whose PARAMS have not arrived is unknown and is
   treated as one. `genesis_refused_outranked`, plus the `deferred_to_sysid`
   this node really stood down for.

The **loser does nothing new**: it keeps the join drive it was already running,
and the moment the winner is a member its op-0x02 is taken and it is admitted on
the ordinary path. There is no second code path for "the node that lost".

### Root cause 2 — the founder's CSID used the falsified slot rule

`coord_genesis_csid()` built `(1 << 16) | (SCSSYSTEMID & 0x3ff)`. rd vms-3a7c had
already settled the assignment rule against the lab oracle — a coordinator hands
out the **round-robin CSV slot** (p. 7-25), never a function of the SCSSYSTEMID
(1986 was assigned slot 3, 1026 was assigned slot 3) — but only the *admission*
path was moved onto it. The capture that suggested the old reading cannot
distinguish the two: both real founders it shows (1025 → 0x00010001, 1027 →
0x00010003) have system ids whose bottom ten bits happen to equal their slots.

It is not harmless. A founder whose SCSSYSTEMID's bottom ten bits fall outside
the grounded 8-slot nodemap byte **could not found at all** — and the demo's own
Node A, SCSSYSTEMID 1987, asks for slot 963 and was refused `NO_SLOT`. The
founder now goes through `coord_next_slot()` and the shared
`coord_csid_of_slot()`, taking slot 1 on a virgin CLUB.

### Also

The discovery window is now armed for **every** VAXCLUSTER=2 node and **before**
the join drive. It used to be armed only on the branch where no join could be
started, so a node that could see a peer at CLUSTER_START never armed it — and
when that join later ended unanswered, the window it needed in order to form had
never been running and could never elapse.

### Tests

`tests/cluster/host/test_cnxman_genesis.c`: the founder takes slot 1 for
SCSSYSTEMID 1987 / 1024 / 1032 (all three refused before); the election elects
**exactly one** of the two symmetric decisions; a VOTES=0 peer is not a rival and
an unknown one is; even the winner asks before it forms, and a node alone still
founds with no evidence at all. `test_cnxman_genesis_negctl.c`: the loser mints
nothing, a CSID-holding peer refuses forming however many rounds were exhausted,
and `NO_SLOT` is re-anchored on the real condition it guards. Every refusal
still compares the whole CLUB byte-for-byte before and after.

## Update 2026-09-23 (vms-151, second half): the cluster formed, and nobody heard it

With the election and the founder's slot in, two fresh OVMX/x86 nodes really did
form a cluster — and the run still read as a hang. Node A founded generation 1,
node D was admitted as CSID `00010002`, `SHOW CLUSTER` on each listed both as
MEMBER — while both consoles had said nothing since the login banner. The
executive was never silent; it was never *audible*.

**Root cause — two right decisions, one level apart.** `exec_console_printf()`
is the seam's OPA0: op (`exec_kbackend.h` §18) and the Linux rind emits it with
`printk` at `KERN_ERR`, chosen because `KERN_INFO` is dropped at the console
level the harness reads. Independently, PID 1's
`ovmx_boot_mute_kernel_console()` (vms-300) lowers the console sink to 3 to keep
module chatter off the VMS boot banner. Linux prints a record iff its level is
**strictly less** than `console_loglevel` — so a sink of 3 swallowed level 3,
i.e. every `%CNXMAN` / `%PEA0` / `%MSCP_CL` line the cluster stack has ever
written. Nothing was wrong in either file; what was missing was a place where
the two numbers have to agree.

Why no earlier capture caught it: every lab node in `tests/lab/captures/` boots
a harness init, not `ovmx_init`, so the mute never ran there and those consoles
show the lines. The muted path is the *product* boot — the bootable image, and
the in-browser demo.

**The fix** is `src/kernel/ovmx_console_policy.h`: both levels in one header,
used by both rinds, with the invariant (`OPA0 level < console mute`) asserted at
compile time on each side — a kernel `static_assert` binds the level the macro
really passes, so the module fails to build if they drift. The sink moves 3 → 4:
EMERG/ALERT/CRIT still reach the console, and so do ERR — the executive's
operator lines plus vms.ko's genuine init-failure `pr_err()`s, which an operator
must see for the same reason. WARNING/NOTICE/INFO/DEBUG stay muted, which is
everything vms-300 asked for. The OPA0: level deliberately stays at ERR rather
than moving to CRIT: `opcom_kmsg_classify()` derives an OPERATOR.LOG record's
severity from this same number, and a membership announcement filed as `-F-`
would be a false severity claim.

**Console hygiene, now that the lines are heard.** Two of them were being
written on a poll and would have repeated forever: the founding refusal (the
gate is asked once a second, so the loser said "another system … takes
precedence" once a second until the winner formed) and the MSCP client's "the
member answered NOT PRESENT HERE" (re-asked every 30 s, because a member may
mount its first served volume later). Both are now edge-triggered on the fact
they report — said when it changes, silent while it holds — and both still
COUNT every occurrence, because a counter is state and speech is not.

**Proof** (`tests/lab/captures/vms-151-genesis-cn2-20260923/`): two OVMX/x86
nodes from cold on one LAN, product boot cmdline, both consoles printing
`%CNXMAN, this node is now a VAXcluster member`, and `SHOW CLUSTER` on each
listing both systems MEMBER with CSIDs `00010001` / `00010002`.

## Update 2026-09-24 (vms-6d3d): the votes are COMBINED, not the founder's own

The predicate above — "quorum met by the node's **own** configured votes" — is
strictly stronger than what VMS does, and it made the **documented two-node
VMScluster unformable**. With `VOTES=1` and `EXPECTED_VOTES=2` on both nodes,
the textbook pair, quorum is `(2+2)/2 = 2` and neither node's single vote
reaches it: `cnxman_quorum_own_votes_suffice()` refused both, forever. Measured
against the shipped browser-demo roster, *no* OVMX node in it could found at
all (OVMXA `EV=2` → quorum 2; OVMXB `EV=3` → quorum 2), so every cluster in the
demo had to be founded by the real-VMS node.

### The oracle

Two real OpenVMS VAX **V7.3** systems, `VOTES=1` / `EXPECTED_VOTES=2` on each,
on a private bridge in a disposable lab pod —
`tests/lab/captures/vms-6d3d-coldform-ev2-20260924/`:

- **Alone, one of them does not form.** VAX1 sat for 18 minutes at
  `%SYSINIT, waiting to form or join a VMScluster system` with **not one
  `%CNXMAN` line**. So the bar is not lowered: a lone node short of quorum
  really does wait, exactly as the old predicate had it.
- **Together, they do.** 3.2 s after VAX2's circuit came up:
  `discovered node VAX2` → `established connection to node VAX2` →
  `proposed formation of a VAXcluster` → both nodes MEMBER, CSIDs `00010001`
  and `00010002`.
- The **same pod**, one SYSGEN digit earlier (`EXPECTED_VOTES=1`), formed
  alone in two seconds — which is what makes the wait attributable to
  `EXPECTED_VOTES` and nothing else.

### What changed

`cnxman_quorum_form_set()` assembles p. 7-6 **step 1**'s proposed set for a
cold formation: this system plus every system whose PARAMS record it really
received over a circuit that is really `OPEN`. (Not the SELECTED walk the
running cluster uses — a formation has no membership yet — but the same INV-6
rule underneath it: an un-advertised VOTES is unknown, never a zero, and an
unreachable system contributes nothing.) `cnxman_quorum_could_found()` is then
p. 7-6 steps 2 and 3 over that set, and `cnxman_quorum_form_votes_suffice()` is
that function asked about **this** node, with the extra condition that this
node's own CSB must be in the set.

The election asks the **same function about the peer**, with the peer's own
advertised VOTES/EXPECTED_VOTES: a candidate is ranked only if it could itself
have formed the cluster being contemplated. `VOTES = 0` falls out of that
predicate rather than being special-cased, a peer whose PARAMS have not arrived
is a rival (unknown, never zero), and a peer this node cannot currently reach is
a rival (there is no set to judge it over). One formula, two subjects — a second
copy is how a node comes to defer forever to a system that could never have
formed anything.

### Not claimed: the founding transition is still single-node

The oracle's VAX2 **never sent a membership request** — it was made a founding
member *in the same transition*, and handed its CSID by it. OVMX founds a
single-node cluster and admits the peer on the ordinary op-0x02 path
milliseconds later (the shape the `EXPECTED_VOTES=1` baseline shows VMS itself
producing in the other configuration). The outcome is the same CN=2 with the
same votes, and both halves run the *existing* machinery, but the
multi-participant founding transition is a real remaining difference. It is
recorded here, in the capture's §4 and in arm 3
(`mixed-ovmx-plus-v73/`, where a real V7.3 system opens exactly such a
formation with an OVMX node and OVMX cannot take its place in it), and tracked
as **rd vms-f29** rather than papered over.

### Tests

`tests/cluster/host/test_cnxman_genesis.c`: the predicate truth table is run
twice — once over a set of one (unchanged, including `VOTES=1/EV=2` → NO) and
once with a peer really in the set, where `VOTES=1/EV=2` + a seen
`VOTES=1/EV=2` peer FOUNDS on two votes, a seen `VOTES=0` peer adds nothing,
and a peer advertising `EXPECTED_VOTES=5` raises quorum to 3 so two votes no
longer suffice. End to end, the pair elects exactly one founder and a node
alone in that configuration still refuses **and says so on OPA0:**.
`test_ineligible_candidate_never_wins()` asserts the refusal a node gives is the
true one: it does not report "another system takes precedence" about a system
that could not have taken it. R4: `RIG_MODE=coldform` on
`tests/qemu/run_cluster_genesis_2node.sh` — both guests `VOTES=1
EXPECTED_VOTES=2`, node A watched for 45 s alone and required to found nothing,
then CN=2 with `role=founder`/`role=joiner` read out of each executive. That
mode FAILS on the pre-fix executive (both nodes `member=0 cn=0 csid=-`), which
is the R4 teeth.

## References

- `docs/compat/facilities/cluster-dlm.yaml` (the four downgraded rows + wire_format)
- `docs/compat/facilities/connection-manager.yaml` (`$join`, `$boot-join`, `$real-vax-join`)
- `src/kernel-core/vms_cnxman.c`, `vms_cnxman_coord_fsm.c`, `vms_cnxman_csb.c`,
  `vms_cnxman_join_fsm.c`, `vms_cnxman_quorum.c`, `vms_cluster.h`
- vms-1ee (the DLM proof this unblocks), vms-3c3 (this item)
- retired pre-pivot member-role items (do NOT resurrect — SCSD.EXE deleted,
  `test_no_scsd_image.sh` forbids it): vms-ae14, vms-f3e, vms-d60, vms-45c, vms-164d
