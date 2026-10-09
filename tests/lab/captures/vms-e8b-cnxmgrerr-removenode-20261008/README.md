# vms-e8b — OVMX answered VAX1's departure commit to VAX2, and VAX2 bugchecked CNXMGRERR

**2026-10-08, pod `vaxlab-3` (the evacuation lane's own rig), cluster group 1.
Reproduced 2/2, deterministically.** The OVMX node is `origin/main` 5b61777e
boot artifacts.

| node | SCSSYSTEMID | VOTES | EXPECTED_VOTES | MAC |
|---|---|---|---|---|
| `VAX1` — real OpenVMS VAX V7.3 under SIMH | 1025 | 1 | 3 | `aa:00:04:00:01:04` |
| `VAX2` — real OpenVMS VAX V7.3 under SIMH | 1026 | 1 | 3 | `aa:00:04:00:02:04` |
| `OVMXE` — booted OVMX/x86_64 under QEMU | 1030 | 1 | 3 | `52:54:00:00:e5:01` |

Q=2, V=3, N=3; stable for minutes before the step. OVMX joined through **VAX2**
(the highest SCSSYSTEMID, rd `vms-e88`), so **VAX1 was never OVMX's join
target** — which is the whole of the defect's precondition.

**The step:** on VAX1, `@SYS$SYSTEM:SHUTDOWN` with the `REMOVE_NODE` option —
the evacuation step rd `vms-ci.6` gates on. VAX1's console:

```
%CNXMAN,  proposing modification of quorum or quorum disk membership
%CNXMAN,  lost connection to system VAX2
%CNXMAN,  timed-out lost connection to system VAX2
%CNXMAN,  quorum lost, blocking activity
```

VAX2's console, within the same second:

```
**** Fatal BUG CHECK, version = V7.3     CNXMGRERR, Error detected by VAXcluster Connection Manager
    Current process = NULL
```

## What is here

| file | what |
|---|---|
| `m3-crashwindow.pcap` | all `0x6007` frames in 20:55:47.0–20:55:52.0 of run **m3** (963 frames), trimmed verbatim from the run's `tcpdump -i br0 'ether proto 0x6007'` |
| `m4-crashwindow.pcap` | the same for run **m4**, 21:13:35.0–21:13:40.0 (100 frames) |
| `m{3,4}-vax1.console.log`, `m{3,4}-vax2.console.log` | the two real VAX consoles, verbatim |
| `m{3,4}-OVMXE.console.log` | the OVMX node's console + kernel log, verbatim |

Run m3's tail also carries VAX2 auto-rebooting on the WRONG system root
(`B/R5:0`, MAC `08:00:2b:d5:24:fe`, claiming 1025) — a lab hazard since fixed
by `BUGREBOOT 0`. It is outside the trimmed window. Run m4 has none of it.

## THE FRAME, in run m4 (`m4-crashwindow.pcap`, indices within the trim)

A `REMOVE_NODE` leave is a **class-0x04** transition — "a node announces its
OWN departure", spec §4(r) — and VAX1 opens it by sending cat-0x01 **op 0x03**
(role 0x20 COMMIT) to **every** other member, on that member's own connection:

```
#88   21:13:37.795420  VAX1 -> VAX2    cat 01 op 03 role 20 cls 04 ep 51249195 txn 5 tok 47903
#89   21:13:37.795646  VAX2 -> VAX1    cat 81 op 03 role 20 cls 04 ep 51249195 txn 5 tok 47903   <- THE ORACLE
#90   21:13:37.795840  VAX1 -> OVMXE   cat 01 op 03 role 20 cls 04 ep 51249195 txn 9 tok 46657
#93   21:13:37.797616  OVMXE-> VAX2    cat 81 op 03 role 20 cls 04 ep 51249195 txn 9 tok 46657   <- WRONG PEER
#94   21:13:37.797994  VAX2 -> ab:00:04:01:01:01   mt 0xb1 last gasp  (CNXMGRERR)
```

- **#89 is the real-VAX oracle for the answer.** VAX2 answers VAX1's commit
  **0.2 ms** later, **on the connection it arrived on**, as the request echoed
  with `body[8] |= 0x80` and `body[18] = 0x01` — class 0x04 and `body[55]`
  **echoed**, not cleared (clearing `body[55]` is op-0x09-specific, §4(p)).
- **#93 is the defect.** OVMX's body is byte-correct — it is what VAX2 itself
  sent VAX1 in #89 — and its envelope (`send-msg# 169`, `ack 394`) is a valid
  continuation of the **OVMX↔VAX2** dialogue. Con.ID pair
  `8e0f0006 -> c6ef000d` is the **OVMX↔VAX2** connection; VAX1's request came
  in on `c6ee000d -> 8e0f0009`. The only wrong thing about the frame is **who
  it was sent to**: a membership-commit response naming a transaction VAX2 had
  never opened. 378 µs later VAX2 bugchecked.
- **VAX1 never got its answer**, so its departure transition stalled, it
  declared `lost connection to system VAX2` and then `quorum lost`.

Run m3 is the same sequence frame for frame (`#949/#950/#951/#956/#958` in the
trim: `VAX1->VAX2 txn 10 tok 6005`, `VAX2->VAX1` answer 0.5 ms later,
`VAX1->OVMXE txn 3 tok 8237` on Con.ID `278f000d -> 79bc0005`, then
`OVMXE->VAX2` on `79bc0009 -> 278f000d` carrying that same `(txn 3, tok 8237)`,
and the last gasp 193 µs later). Note that in m3 OVMX answered VAX1 with
nothing but a `cat 04` credit ack (`#957`) — the commit response VAX1 was
waiting for had gone to VAX2.

## Why neither OVMX console says anything

`m4-OVMXE.console.log` logs nothing at all for the quorum-modification
transition and notices VAX2 only ~20 s later as a path loss. That is correct
for the code as it stood: the frame was built, accepted by SCS and sent — there
was nothing to report. The defect was invisible from this node's own side, and
is only visible on the wire.

## The same defect, fourteen days earlier and harmless

`tests/lab/captures/vms-4f0-cn3-relay-20260924/run1-fixed/cn3.pcap` frame 2404
and `.../run2-base-control/cn3.pcap` frame 2361 each carry one `cat 81 op 03`
from OVMXB (`52:54:00:00:4f:0b`) to **OVMXA** (`52:54:00:00:4f:0a`) answering a
request the real VAXC (`08:00:2b:01:97:7e`) had sent OVMXB. It crashed nobody
because the misdirected frame landed on another OVMX node, which tolerates it.

## Reproducing the measurement from these files

`tools/cluster/cm_wire_safety_audit.py` grew a finding class for exactly this
(**S16-RESP-PEER-CROSSED**, the response-correlation rule):

```
tools/cluster/cm_wire_safety_audit.py \
    tests/lab/captures/vms-e8b-cnxmgrerr-removenode-20261008/m4-crashwindow.pcap
```

Its calibration over this repo's whole capture library is in that file's
grounding table: **409 judged cat-0x01 responses across 62 captures, 2
findings, both OVMX, zero from any real-VAX responder.**

## The fix, and what a lab re-run must show

The fix is in `src/kernel-core/vms_cnxman_join_fsm.c`: a response leaves on the
connection its request arrived on (`join_emit_reply()` → `ops->respond`),
stamped from that connection's own CSB, which is what the barrier FSM has
always done and what §4(p) says ("answer on whichever the request arrived on").

A re-run of this exact step must show, on the wire:

1. `OVMXE -> VAX1  cat 81 op 03 cls 04 txn 9 tok 46657` — the answer going
   **back to VAX1**, on `8e0f0009`, within a few ms;
2. **no** `cat 81 op 03` from OVMXE to VAX2 at all;
3. VAX2 **not** bugchecking, and both survivors completing VAX1's departure
   transition (`%CNXMAN, removed from VAXcluster system VAX1` on VAX2,
   `completing VAXcluster state transition`);
4. OVMX's `cnxman_join` transcript showing `replies_offtarget` ≥ 1 and
   `replies_unaddressed` 0;
5. `cm_wire_safety_audit.py` reporting **0** S16 findings on the new capture.
