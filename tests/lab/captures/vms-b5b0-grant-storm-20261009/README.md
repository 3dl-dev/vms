# vms-b5b0 — the DLM request storm, 2026-10-09 (evacuation lab, vaxlab-3)

What this directory holds, and what was measured from it. Nothing here is a
retelling: every number below was computed from `vms-b5b0-storm-window.pcap`,
and the two frames the fix is proven against ride as codec fixtures
(`tests/cluster/host/fixtures/dlm-real-enq-{request,grant}.spec` and
`dlm-storm-{request,answer}.spec`).

## The run

| | |
|---|---|
| build | `evac/int-4` (`57bc0dc2` = main + #1578@3189f08f + #1582), booted OVMXE |
| cluster | VAX1 + VAX2 (OpenVMS VAX V7.3, LOCKDIRWT 0) + OVMXE (LOCKDIRWT 1), VOTES 1/1/1, EXPECTED_VOTES 3, group 1 |
| join | clean at 27 s; `%DLM, lock directory weight vector: 1 entries over 3 systems, 1 of them this node's` |

## What happened

1. An OVMX process took **NL** on `EVAC$WORKLOAD` (group 1, supervisor mode), so
   **OVMX mastered** the resource.
2. ~10 s later VAX1 `$ENQW`'d **EX** on it. NL conflicts with nothing, so the
   request was grantable immediately and OVMX answered.
3. VAX1 re-sent the **same** request **65,356 times in 63.7 s** — 1026/s, median
   gap **0.362 ms** — and OVMX answered **65,340** of them with the **same**
   bytes. No bugcheck on either VAX. An operator stopped it.

## Why (decoded from the frames, not inferred)

OVMX's answer carried:

- the two lock handles **in each other's slots** — `body[20:24]` held the
  request's own PID-form placeholder echoed, `body[24:28]` held **OVMX's** master
  handle. VAX1's own handle (`0x0f0003ce`, which it had sent at `body[24:28]`)
  appeared **nowhere** in the reply, so VAX1 had nothing to correlate the
  completion to;
- **no grant record**: `body[28]=0x00` and `body[32:36]=0`, so `body[34]` — the
  outcome byte a real answer carries `0xfa`/`0xf9`/`0xf8` in — read `0x00`;
- the **granted mode** at `body[30]`, a byte 38 of 38 real grants clear;
- zeros at `body[36:52]`, where the master resource's value block belongs.

A real VAX↔VAX grant from the same capture (SCA frames 1 and 2, 155 µs apart) is
the reference the fix is measured against. OVMX's corrected builder reproduces
it on **126 of 132** body bytes — every byte the DLM codec owns. The six it does
not write are `body[0:4]` (the connection manager's transaction envelope) and
`body[52:54]` (an SCS-layer word), both left zero rather than minted.

## Files

| file | what it is |
|---|---|
| `vms-b5b0-storm-window.pcap` | 167 SCA frames: every VAX↔VAX DLM frame of the run (the reference grants) plus the first 40 frames of the storm. Trimmed from the 34 MB full capture, whose sha256 is `23e4524f5f0fabb942d5aa67856c461d2e2fe3c67c00942ffb6d0f529cd87d4b`; the trimmed file's own digest is in `docs/clean-room/reference-captures.sha256`, which is what the fixture loader checks custody against. |
| `real-request.frame.hex` / `real-grant.frame.hex` | the real VAX1→VAX2 request and VAX2→VAX1 grant (SCA frames 1 and 2), one hex line each |
| `storm-request.frame.hex` / `storm-answer.frame.hex` | one of VAX1's repeated requests and OVMX's answer to it (SCA frames 2081, 2082) |

## What the lab must show next (not proven here)

A booted OVMX master granting a real VAX's `$ENQW` on a resource it masters,
with **zero retransmits**: one op-0x01 in, one cat-0x82 out, the VAX's `$ENQW`
completing, `SHOW LOCKS` on the VAX naming the granted mode, and 0 bugchecks on
either VAX. The `%DLM, a system is re-sending one lock request this node has
already answered identically` console line must NOT appear — if it does, the
answer is still not being understood, and the guard is what kept the peer alive
while that was true.
