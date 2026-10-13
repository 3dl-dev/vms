# vms-52d: pre-registered before any run (2026-10-13)

## The question

In every OVMX-founded run, each booting real VAX sends OVMX exactly one cat-0x0b op-0x01
whose body carries [16:19] = `10 02 01` (role 0x10, class 2) and [28:32] = 1. OVMX does not
answer it, and the VAX never reaches `Username:`. Across the whole capture corpus (repo
captures, rm380 3-VAX remaster pcaps, the d52c/db1 fires), no real VAX sends that form to
another real VAX. VAX-to-VAX traffic carries the role form only on op 0x02.

## Hypothesis H1 (LOCKDIRWT)

OVMX advertises LOCKDIRWT 0 in its PARAMS (body[26:28]); every lab VAX advertises 1. H1 says
a booting VAX sends this form to a member that advertises LOCKDIRWT 0 while it advertises
more, whoever founded the cluster.

## Arm A (pod lab52da, no OVMX node)

VAX1 founds by itself, booted conversationally with `SET VOTES 1`, `SET EXPECTED_VOTES 1`,
`SET LOCKDIRWT 0`. This is OVMX's position: founder, coordinator, LDW 0. Once VAX1 is a
member, VAX2 (SYS1) and VAX3 (SYS2) boot together with the golden parameters (LDW 1).

Under H1:
- A1: VAX2 and VAX3 each send VAX1 at least one cat-0x0b op-0x01 with body[16:19] = `10 xx 01`.
- A2: VAX1 answers each with cat 0x8b op 0x01. Its body is recorded in full; this answer
  is the oracle for vms-52d.
- A3: every VAX reaches `Username:`, with 0 bugchecks.

If A1 fails, H1 is refuted. The next arm then varies what OVMX advertises and a VAX does
not: PARAMS body[64:72], [72:76] = 0x10, [76:80] EXPECTED_VOTES, [84:88], and the 01/14
model text.

## Arm A result (recorded before arm B is registered or run)

A1 **failed**: no VAX sent VAX1 a role-form 0b/01, so H1 is refuted. A3 held: all three VAXes
became members, with 0 bugchecks.

Arm A, the corpus, and the db1-r2 timeline show what the role form is. A 0b/01 request
writes only body[8:12], [24:28] (0) and [28:32] (1). Every other byte is left over from the
last message on that connection. `stale.py` shows this: VAX2's 0b/01 to VAX1 at db1-r2
303.777 differs from VAX1's preceding 82/01 at [10:12], [24:26] and [27:29] only. Toward
OVMX, the leftover is OVMX's own 0x81/0x0b barrier ack, which is the `10 02 01` at [16:19].
VAX1's copy still holds 'SCS$' / 'UP' from its 'SCS$DIR_LOOKUP' step. So OVMX received the
**ordinary** 0b/01 that every booting VAX sends every member; VAX2 sent VAX1 one in the same
millisecond. OVMX is the only member that never answers it.

The real answer (28 corpus pairs) echoes the request with body[8] |= 0x80 and writes body[28:32]
and body[36:40] as a pair: (1, 1) in 18 pairs and (0, 0) in 10. Nothing else changes. In every
pair the (1, 1) answerer both became a member earlier and has the lower SCSSYSTEMID, so the two
candidate rules are confounded:
- R-old: answer (1, 1) when the answerer became a member before the requester, otherwise (0, 0).
- R-id: answer (1, 1) when the answerer's SCSSYSTEMID is lower than the requester's.

## Arm B (pod lab52db): OVMX's exact position, which separates R-old from R-id

VAX3 (SCSSYSTEMID 1027, the highest, like OVMXE's 1030) founds with `SET VOTES 1`,
`SET EXPECTED_VOTES 1`, `SET LOCKDIRWT 0`. VAX1 (1025) is held at SYSBOOT> until VAX3 is a
member; then VAX1 and VAX2 (1026) boot with the golden parameters.

- B1: each of VAX1 and VAX2 sends VAX3 one 0b/01, and VAX3 answers each with 8b/01.
- B2 under R-old: VAX3's answers carry (1, 1). Under R-id: (0, 0).
- B3: every VAX reaches `Username:`, with 0 bugchecks.
