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
