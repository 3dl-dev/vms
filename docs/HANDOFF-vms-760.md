# Handoff — vms-760 (OVMX → `SHOW CLUSTER` MEMBER)

**Written 2026-07-30 at the end of a long session. Read this, then `rd show vms-760`.**
Everything below is *state and pointers*. The protocol knowledge is in the spec — do not
re-derive it:

- **`docs/cluster-protocol-spec.md` §4(m)** — SCS connection lifecycle (the `op` verb set,
  the msgtype phase rule, connect-class, the confirm invariant). **Read this first.**
- **`docs/cluster-protocol-spec.md` §4(n)** — MSCP disk-client command layer.
- **`docs/cluster-protocol-spec.md` §5(z)** — the open frontier, stated precisely.
- **`docs/design-cluster-join-choreography.md`** — the join choreography + live results.

> ## ⚠ UPDATED 2026-07-30 (session e) — the "one open question" below is ANSWERED
>
> **It was not a CM-level admission predicate. It was two ordinary defects**, both
> found by diffing OVMX against the reference instead of theorising about it:
>
> 1. **The joiner never sent the op-3 CONFIRM on its own `VMS$VAXcluster` VC.**
>    A half-open VC binds the Con.ID pair and then *silently discards* the SYSAP
>    dialogue — which looks exactly like a policy refusal from the outside. That
>    is why it was misread. See spec §4(m).
> 2. **Directed HELLOs put the peer's HW MAC in abs 16 instead of its
>    cluster-logical address.** Invisible in a 2-node lab (VAX1's HW MAC *is* its
>    logical address); fatal for VAX2/VAX3. See spec §4(a).0.
>
> With both fixed: all three members complete the config exchange with OVMX and
> open connections back, and OVMX sends the deferred admission `0x02`
> (spec §4(o)) byte-identical to the reference. **Still `NEW`** — no member acks
> the `0x02`, and the next divergence is located precisely in §5(z).
>
> **Do not trust a negative gathered late in a test session.** Stale `NEW` CSBs
> accumulate in the member; after ~16, `SHOW CLUSTER` rendered an *empty table*.
> Clear them and re-run before believing a "no response" result.
>
> Read **spec §4(a).0, §4(m), §4(o), §5(z)** and the `vms-760` progress notes.
> Everything below this box is the pre-session-e state, kept for the lab
> procedures and pointers, which are still accurate.

## Where it stands

`OVMX_JOIN_SEQ=1` drives the full joiner-side choreography against the live lab and the
real VAX **accepts every connection**:

| step | result |
|------|--------|
| own `SCS$DIRECTORY` connect | VAX1 op-2 ACCEPT |
| dir confirm + `MSCP$TAPE`/`MSCP$DISK`/`VMS$VAXcluster` lookups | answered |
| own `MSCP$DISK` connect | **`MSCPBOUND`** |
| MSCP disk discovery (SCC ×2 + GUS walk) | **units 4000-4003 AVAILABLE + OFFLINE terminator — identical to a real VAX** |
| own `VMS$VAXcluster` VC connect | **`JOINBOUND`** |
| MODEL+PARAMS add-member burst on our own VC | sent |

**`SHOW CLUSTER` reads `NEW`, not `MEMBER`.**

## The one open question

VAX1 **does not reciprocate**. It never answers the config burst, and — the diagnostic
tell — it **never opens its own connections back to OVMX**, which it does to a real
joiner within ~15 ms.

⇒ Admission is gated **above the SCS frame layer**. The connection layer is solved; do
not keep diffing connect frames. Diff what a real node *presents*: the `0x41` START
**body**, the HELLO/channel advertisement (`0xb3`/`0xa0`), the identity/config a genuine
VMS node carries. See §5(z).

## Reference capture (the crown jewel)

`~/vax/cluster/captures/vax3-2to3-established-join-20260730.pcap` — a **real VAX3**
joining the **running** cluster and reaching MEMBER. VAX3 = `08:00:2b:11:22:33`,
VAX1 = `aa:00:04:00:01:04`, VAX2 = `08:00:2b:78:56:b9`. Every other join specimen is a
1→2 *formation* and misled earlier sessions — **use this one** for anything about an
established join.

Best OVMX run to diff against it: `/home/baron/.claude/jobs/678334fd/tmp/d94-disc3.pcap`
(archived — see the clean-room archive if the job dir is gone).

## Lab

Live **3-node** cluster: VAX1 + VAX2 + VAX3, all MEMBER. VAX3 is a permanent lab node now
(root `[SYS2]`, `SCSSYSTEMID=1027`, `VOTES=0`, tap3, disk-based `vax.ini`).

- Reset to pristine 2-node: `bash /home/baron/.claude/jobs/678334fd/tmp/golden-reset.sh <id>`
  (stops SIMH → restores `d0/d1.dsk.2node-golden.bak` → boots vax1, logs in, boots vax2).
  Backup taken before VAX3 was added: `data/d0.dsk.pre-vax3.bak`.
- **Do a golden reset before any MEMBER attempt** — stale half-joined OVMX entries
  accumulate in VAX1's `SHOW CLUSTER` and eventually block even the `0x41` START.
- Test loop:
  `OVMX_JOIN_SEQ=1 sudo env OVMX_SYSGEN_PATH=<fresh store, novel SCSSYSTEMID> build-d94/bin/SCSD.EXE --connect --duration 100 --iface br0`
  Fresh store: `/tmp/clean-vax1-test/mk_sysgen <path> <NODE> <sysid>`. Drive `SHOW CLUSTER`
  via `printf 'SHOW CLUSTER\r' > /tmp/clean-vax1-test/vax1.log.in`.
- **Gotcha that bit twice:** never `pgrep -f <pat>` / `pkill -f <pat>` when `<pat>` also
  appears in your own command — it matches your shell and kills it (exit 144). Use
  `pgrep -x vax` and explicit PIDs.
- `/tmp` is `noexec` here: run scripts as `bash script.sh`, not `./script.sh`.

## Code

Branch `worktree-760-active-directory`. All joiner-side work is gated behind
**`OVMX_JOIN_SEQ`**; the default path is untouched (Rule 9). 10/10 vmsscs tests.

- `src/vmsscs/scs_mscp.{c,h}` — MSCP builders/parser (byte-exact, unit-tested).
- `src/vmsscs/scsd.c` — `ps_mscp_disc()` drives discovery; `scs_reflect_credit()` answers
  op 6/8 on every path; the `JS_*` sequencer drives the 8 steps.
- `OVMX_PURE_SERVER` is the **superseded** model (member-driven). It reaches NEW but is
  architecturally wrong — the joiner drives. Kept for comparison only; don't build on it.

## Clean-room

`docs/clean-room/` — attestation, self-verifying SHA-256 manifests, decoder tools,
`retain.sh`. **Run `bash docs/clean-room/retain.sh <session-id>` at the end of every RE
session and commit the refreshed `*.sha256`.** Archive:
`~/vax/clean-room-archive/2026-07-30-session-f0b8efb2/` (43 MB). Every RE'd byte must stay
traceable to a capture frame + timestamp in an in-source comment.
