# Analysis — `vms-e81` run `by11`: the reciprocal fired, and VAX3 still did not join

> Written 2026-07-31 from **two independent capture analyses** that were run
> without knowledge of each other and **converged on the same root cause and the
> same field map**. Where they agree the finding is recorded as GROUNDED; where
> only one saw something it is marked as such.
>
> Clean-room: every claim is derived from observed wire bytes plus the field map
> already in `docs/cluster-protocol-spec.md`. No VSI/HPE source or binary was
> consulted, disassembled or decompiled.

Specimens (under `/home/baron/vax/cluster/captures/`):

| tag | file | what it is |
|---|---|---|
| `by11` | `ovmx-e81-by11-reciprocated-still-no-join-20260731.pcap` | 8574 frames, 707.9 s. VAX1+VAX2+OVMX settled; VAX3 boots in at ≈ +53 s and never joins. |
| `by10` | `ovmx-e81-by10-vc-bound-vax3-stalls-20260731.pcap` | same test, before the reciprocal fix |
| `ctl` | `control-member-meets-late-node-20260730.pcap` | pure VMS: VAX1+VAX2, VAX3 boots in and **joins normally** |
| `v3est` | `vax3-2to3-established-join-20260730.pcap` | second pure-VMS control of the same event |
| `ci1` | `formation-ci1-joinwindow.pcap` | 1-node cluster meets a joiner |

Offsets are **absolute frame offsets**; `body[n]` = abs `72+n` (spec §4(j)).

---

## 0. The answer in one paragraph

**No frame is missing.** VAX3 received every message class from OVMX that the
control shows a real member sending, in the right order, on the right connection,
inside the reference latency (+0.1 ms vs the reference's 0.3–0.9 ms). The SYSAP
header is correct — `smsg=1 amsg=0` then `smsg=2 amsg=0`, byte-exact against 4
reference reciprocals, which **refutes the handoff's prime suspect #1.**

**What is wrong is the CONTENT.** OVMX's `cat 0x01 op 0x01` is a byte-for-byte
replay of a **JOINER's** cluster-parameters message — 131 of its 132 body bytes
are identical to the `op 0x01` VAX3 itself emits while it is *not yet in a
cluster* — sent while OVMX is sitting in the cluster as a MEMBER. Every field
that distinguishes a member from a joiner reads zero. OVMX tells the newcomer:
*"I am in no cluster, I know of 0 members, no formation time, no transition time,
and I was admitted on 1-JAN-2001."* VAX1 and VAX2 tell it, in the same second:
*"3 members, state-seq 4, formed 02:02:11.14, last transition 02:05:31.79"* —
where that last transition **is OVMX's own admission**.

OVMX then compounded it by sending VAX3 a **joiner** `cat 0x01 op 0x02` — the
add-member request — i.e. an established member asking a node that is not yet a
member to admit *it*. No member sends `op 0x02` in any control capture.

---

## 1. The member-vs-joiner field map (GROUNDED, both analyses)

A census of every `cat 0x01 op 0x01` in both captures splits **6 member frames vs
6 joiner frames with zero residuals**, and the member values track cluster size
across a controlled variation *inside a single capture* (by11 VAX2 says 2 at
+2.5 s and 3 at +53.9 s, the change being OVMX's own admission at +9.5 s).

| abs | body | field | joiner | member | OVMX sends |
|---|---|---|---|---|---|
| 84 | `[12]` | **member flag** (`[12:16]` = `0x00005021` / `0x00005000`) | `0x00` | `0x21` | `0x00` ✗ |
| 90 | `[18]` | **current member count** | `0` | 2 in a 2-node, **3** in by11 | `0` ✗ |
| 92 | `[20:22]` | unknown, constant `1` everywhere | 1 | 1 | 1 ✓ |
| 94 | `[22:24]` | **VOTES** (§4(j)) | 0 | VAX1 `1`, VAX2 `0` | `0` ✓ (non-voting) |
| 100–107 | `[28:36]` | **cluster formation time**, VMS quadword | zeros | by11 `02:02:11.14` (= VAX1's own) | zeros ✗ |
| 108–115 | `[36:44]` | **last state-transition time**, VMS quadword | zeros | by11 `02:05:31.79` | zeros ✗ |
| 116–119 | `[44:48]` | **cluster state-sequence** (u32) = member count + 1 | 0, then the learned value | 3 (2-node), **4** (by11) | `0` ✗ |
| 136–143 | `[64:72]` | **sender's own admission / incarnation**, VMS quadword | `2001-01-01 00:00:00` | VAX1 `02:02:11.14`, VAX2 `02:03:31.33` | the joiner sentinel ✗ |
| 154 | `[82]` | unknown; tracks the same split | `0x2a` | `0x2b` | `0x2a` ✗ |
| 156–159 | `[84:88]` | per-boot noise (§4(j) already flags it) | — | — | — |
| 160–167 | `[88:96]` | version `"V7.3    "` | same | same | **byte-exact ✓** |

**The `"V7.3"` field is not a defect.** Nor is VOTES. Nor the SYSAP header.

Two cross-checks that make the timestamps load-bearing rather than cosmetic:

- `body[36:44]` in by11 **changed to OVMX's own admission time** when OVMX
  joined (`scsd-by11.log` logs the deferred `op 0x02` at 02:05:31.896 and the
  COMMIT at 02:05:31.906). So it is a cluster-wide fact the members agree on.
- The joiner sentinel `00 80 4a 3f 0e 57 9f 00` decodes as **exactly
  2001-01-01 00:00:00.000** in the VMS 1858 epoch — a round "not yet set" value.
  OVMX replays it *while claiming to be a member*, i.e. it tells VAX3 it was
  admitted 25 years before the cluster it is in was formed.

> **Decisive single fact:** byte-compare OVMX's `op 0x01` (sent **as a member**)
> against VAX3's `op 0x01` (sent **as a joiner**, 2.2 s earlier, same capture) —
> **131 of 132 bytes identical**, and the one difference is the field §4(j)
> already classifies as per-boot noise. OVMX is emitting a captured joiner body
> verbatim. It worked during OVMX's own join *because that is what OVMX was*; the
> encoding was never updated when OVMX became a member.

## 2. VAX3 acknowledged everything — then went quiet

VAX3 credited, acked and answered every OVMX frame at the SCS layer (credit
returns, `op 9` CRED-RSP, directory teardown `op 6`/`op 7`, and a `cat 0x04`
acking OVMX's `op 0x02`). Its **last SCS frame to OVMX is at +62.83**; for the
remaining **642 s** it sends nothing, while `0xb3`/`0xb4` channel HELLOs continue
to the final frame. The VC and the datalink never break.

**VAX3 sent zero `cat 0x01 op 0x02` to anyone** for the whole run. It did not
propose to the wrong node — it never proposed.

## 3. The control, and the checklist that comes up complete

In `ctl`, every frame VAX3 receives between its own `op 0x14` and its `op 0x02`
is: credit-return, `op 9` CRED-RSP, the member's `op 0x14` + `op 0x01`, ECHO +
ACCEPT4 for its `MSCP$DISK` connect, and the directory teardown. **No third
config message. No `op 0x02` from a member. Nothing on another connection.** The
member's whole contribution is two frames, and the newcomer's `op 0x02` follows
the **last** member to reciprocate by 110 ms.

Checked row by row against `by11`: **every item is PRESENT**, including OVMX's
reciprocal at +0.1 ms. VAX1 and VAX2 also reciprocated (in member form). The
first thing present in the control and absent in `by11` is not a message — it is
the member-form *content* of `cat 0x01 op 0x01`.

## 4. Handoff defect #1 reproduced — real, but 10.75 s too late to be the cause

VAX3 answered OVMX's `MSCP$DISK` connect with `op 1` ECHO + **`op 4` ACCEPT4**;
OVMX owed an `op 5` CONFIRM5 and never sent one, then retransmitted the connect
~60 times over 178 s **replaying `ss=23`**, a value VAX3 had already consumed.
After a one-off `ss=24` the counter never advances again: from +66.81 to the end
of the run OVMX is structurally unable to send VAX3 anything.

| event | t | Δ from the reciprocal |
|---|---|---|
| OVMX reciprocal | **+56.054** | 0 |
| control's `op 0x02` deadline (+110 ms) | +56.164 | +0.110 |
| slowest reference latency (`v3est`, +4.4 s) | +60.45 | +4.4 |
| first replayed `ss=23` — **freeze begins** | **+66.807** | **+10.75** |

Both reference windows had long closed while OVMX's send path was healthy.
**Defect #1 did not cause this stall — but it makes it unrecoverable**, so fixing
the content without fixing it would only move the stall. *(Fixed in `0607adf`.)*

## 5. Five SYSAP frames on the wire where the reference sends two

| # | frame | t | cat/op | smsg/amsg | verdict |
|---|---|---|---|---|---|
| 1 | 2889 | +56.054 | `0x01/0x14` | 1/0 | **correct** — only flaw is the 17-byte model string |
| 2 | 2890 | +56.054 | `0x01/0x01` | 2/0 | **THE BUG** — joiner-form body |
| 3 | 2945 | +62.808 | `0x04/0x00` | 3/2 | **wrong** — poll-tick ack of the config, 6.75 s late. The reference member's first `cat 0x04` acks the newcomer's **`op 0x02`**, 0.4 ms after it |
| 4 | 2946 | +62.808 | `0x01/0x02` | 4/2 | **wrong and actively harmful** — the *joiner's* admission request, from an established member to a newcomer |
| 5 | 3006 | +66.807 | `0x04/0x00` | 5/3 | **wrong** — acking an ack; the reference never does |
| 6 | — | — | — | — | **never transmitted** — counted by the peer table, absent from the wire, consistent with the §4 freeze |

The poll tick, not the protocol, is choosing when OVMX speaks.

## 6. What the evidence does NOT settle

1. **Which reading is true:** (a) VAX3 requires *every* peer to advertise a
   consistent member-form cluster state, or (b) VAX3 aims its `op 0x02` at the
   **last peer to reciprocate** (OVMX here) and refuses that peer because it
   advertises non-membership, with no fallback. Both fit every byte in both
   captures. **Both are fixed by the same change** — but the next run must be
   able to tell them apart (§8).
2. **The `op 0x02` peer-selection rule.** Across three joins the target is VAX2
   every time, regardless of ordering — so it is a property of VAX2, not of the
   order. Candidates: latest own-admission time, or VOTES 0. Not separable.
3. **Which of the differing bytes VAX3 actually keys on.** A passive capture
   cannot show a decision that emits nothing.
4. `body[12:14]`'s `0x21` bits and `body[82]`'s `0x2b`/`0x2a` are grounded as
   member/joiner discriminators but not decomposed. Set them because the
   reference does, not because we know why.
5. **One analysis saw** `ctl` frame 12067 — a post-join `cat 0x01 op 0x01` with
   completely different content. **Do not assume this field map holds for it.**
6. **One analysis saw** VAX1's `op 0x14` padding carrying leaked console text
   (`%LICENSE-F-EXCEEDED…`). Real VMS does not zero that tail; OVMX does. Read as
   uninitialised buffer content, not a field, but unproven.
7. Whether OVMX's stray `op 0x02` (frame 2946) actively contributes or is inert.
   VAX3 acked it and did nothing else.
8. **Defect #3 needs re-deriving, not assuming.** `by11` frame 2980 shows **VAX1
   doing the same connect-back** at +63.89, so the "~11 minutes early"
   characterisation from the `by10` analysis is not reproduced here.

## 7. Implementation checklist

When OVMX is an established MEMBER and a newcomer sends `op 0x14` + `op 0x01` on
a VC it opened to us:

1. Reply `op 0x14`, `smsg=1 amsg=0`, within ~1 ms — **DONE**
2. …carrying `abs 88 = 0x15` + `"VAXserver 3900 Series"` — **WRONG** (we send
   `0x11` + `"OVMX Cluster Node"`). *Authenticity tell, not a blocker — VAX1/VAX2
   accepted it during OVMX's own join.*
3. Reply `op 0x01`, `smsg=2 amsg=0`, within ~1 ms — **DONE**
4. …with `abs 84 = 0x21`, `abs 90 = <live member count>`,
   `abs 116 = <cluster state-seq>`, `abs 154 = 0x2b` — **MISSING**
5. …with `abs 100..107` = cluster formation time and `abs 108..115` = last
   state-transition time, VMS quadwords **learned from the members' own
   `op 0x01`, never computed** — **MISSING**
6. …with `abs 136..143` = our own non-zero admission quadword, not the replayed
   `2001-01-01` sentinel — **WRONG**
7. Send **nothing else** to that newcomer: no `cat 0x01 op 0x02`, no poll-tick
   `cat 0x04` acks, no connect-back until the transition completes — **WRONG**
8. Answer an `op 4` ACCEPT4 with `op 5` CONFIRM5; never replay a consumed
   `send_seq` — **DONE in `0607adf`**

Items 4–6 are one change: **stop emitting the captured joiner body once we are a
member.** That single edit moves all of the differing bytes.

> **Rule 10 caution for whoever implements 5:** `body[36:44]` is a cluster-wide
> fact — in `by11` it changed to OVMX's own admission time. **Copy it from a
> member's `op 0x01`. Never compute it.**

## 8. The next experiment

**Ship items 4+5+6+7 as one change** and run `tools/bystander.sh` unchanged. Do
**not** bundle item 2 — VAX3 already accepted the OVMX model string during OVMX's
own join, so it is an authenticity tell and bundling it destroys attribution.

**Success oracle, in order:** (1) OVMX's reciprocal carries `abs 90` = 3 and three
non-zero `0x00bc04xx…` quadwords; (2) **VAX3 emits `cat 0x01 op 0x02`** — the one
frame this whole item turns on; (3) VAX1's console names VAX3; (4) OVMX logs a
second `XITDONE`.

**And record WHICH PEER VAX3 aims that `op 0x02` at** — it is the discriminator
for §6.1. To **OVMX** (the last reciprocator) ⇒ reading (b), peer selection is
last-reciprocator. To **VAX1 or VAX2** ⇒ reading (a), cross-peer consistency was
the gate. Either way it also settles the standing `OVMX_CFG2_ALL` fan-out
question in spec §5(z) from the other side of the wire.

**If VAX3 still emits no `op 0x02`, do NOT iterate on more body bytes.** Run the
peer-selection bisect instead: repeat with `abs 136..143` set later than VAX2's
admission (making OVMX the newest member), then earlier than VAX1's (the oldest).
If VAX3 proposes only in the "newest" arm, the rule is most-recently-admitted and
the timestamp is load-bearing. If it proposes in neither, the rule is not on the
wire and the next lead is the SDA / CNXMAN OPCOM oracle on VAX3 itself, not
another capture.
