# vms-db1: the kind-10 close answer (registered before any fire)

Registered 2026-10-12, before the first lab fire of a build that answers kind 10.

## Derivation
The rule below was derived black-box (Rule 8) from real VAX-to-VAX pairs in the lab corpus: `tests/lab/captures`, `cluster/`, and the lane captures under `/tmp/evac`. There are 166 kind-10 closes with a real answer. 76 of them have the full exchange: the record the recipient pulled (0x08/0x0d) and the record it wrote back (0x05).

A kind-10 close is a cat-0x06 op-0x00 with body[26] = 10 and length 0x88. It moves one 136-byte record. That record had a constant shape in 79 of 79 pulls:
- rec[8:12] = `88 00 65 64`
- rec[16:32] = `01 00 1a 00 00 00 52 00 00 00 1a 00 00 00 52 00`
- rec[80:84] = 0

The answer state follows one rule, with 162 of 166 consistent:
- **3** (an immediate cat-0x86, no pull): the recipient has not yet sent its own kind-10 close and does so later (83/83). Of the remaining 4 "3" answers, the recipient never asks within the capture; they are unexplained, and they are why the count is 162, not 166.
- **5** (pull the record, write it back, then a cat-0x86): the recipient already sent its own (51), or never sends one in the capture (28). In 0 cases did a node answer 5 and then send its own.

Each of the 76 write-backs equals the pulled record except at three bytes, plus rec[0:8]:
- rec[82] := 1
- rec[102] := 2
- rec[106] := 8

This holds in 45 of 45 three-node pairs and 31 of 31 two-node pairs. rec[0:8] holds a value that looks like the recipient's own memory. It is ungrounded and left as the requester sent it, as for rd vms-1f64's kind 6.

OVMX never sends a kind-10 close of its own, so it is always in the "already done" class and answers 5.

## Lab prediction (OVMX founds first, then two VAX V7.3 boot; the labd52c recipe)
- **Q1.** Every kind-10 close a VAX sends OVMXE is answered within 1 s with: a pull of 136 bytes, a write-back equal to the pulled record except rec[0:8], [82]=1, [102]=2 and [106]=8, and then a cat-0x86 with state 5.
- **Q2.** Both VAXes finish startup: each console shows `Username:`, and after login `F$GETSYI("CLUSTER_NODES")` = 3.
- **Q3.** 0 bugchecks. No VC between any two nodes drops while no SIMH pause is in progress.

Falsifier: a VAX that retries the kind-10 close after a state-5 answer, or that never reaches `Username:`.

## Addendum: kind 5 (registered 2026-10-12, after fire db1-r1 and before db1-r2)

Fire db1-r1 confirmed Q1. Each VAX's kind-10 close got state 5 within 40 ms. Both VAXes then sent OVMXE a kind-5 close (136 bytes), which this build withheld, and VAX1 still had no `Username:`.

Real kind-5 pairs in the corpus: 3,313 answers.
- **4** after a pull, with nothing written back: 3,069.
- **3** with no pull: 172. Every one came from a node that still had its own close to send, the same rule as kind 10.
- Records are 136 + 64n bytes. The largest measured is 1,480.

OVMX's rule: pull the record whole, write nothing back, answer 4. A record larger than OVMX's 2,048-byte buffer is not pulled; that case is counted and withheld.

- **Q4 (db1-r2).** Every kind-5 and kind-10 close a VAX sends OVMXE is answered: 4 after a pull, or 5 after the measured write-back. Both VAXes reach `Username:`, and `CLUSTER_NODES` = 3. 0 bugchecks.
