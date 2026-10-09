# vms-025 — the mixed-cluster DLM refusal, captured on a real OpenVMS VAX cluster

**Captured 2026-10-09 04:27–04:32Z, pod `vaxlab-3`** (evacuation lane's own lab —
[[own-lab-per-lane-model]]; the shared oracle was untouched). Clean-room: every
byte here is observed wire/console behaviour of a real system; no VSI/HPE source,
binary or disassembly was consulted.

## The bed

| node | SCSSYSTEMID | kind | LOCKDIRWT | VOTES | EXPECTED_VOTES |
|------|-------------|------|-----------|-------|----------------|
| VAX1 | 1025 | real OpenVMS VAX V7.3 | 0 | 1 | 3 |
| VAX2 | 1026 | real OpenVMS VAX V7.3 | 0 | 1 | 3 |
| OVMXE | 1030 (MAC `52:54:00:00:e5:01`) | OVMX, branch `evac/int-2` @ `0765689` | **intended 1, actually 0** | 1 | 3 |

Cluster group 1. OVMX build = `origin/main` (incl. #1568 #1572 #1575 #1564) +
#1571 (EVACWL) + #1577.

## The sequence

1. `04:30:36` OVMXE: `EVACWL STANDBY NL=75` — an OVMX process takes **NL** on the
   user-mode resource `EVAC$WORKLOAD` first, so OVMX should be both its
   **directory** and its **master**.
2. `04:31:04` VAX1: `EVACWL` (direct **EX**, forever) — VAX1 sends **one**
   cat-0x02 op-0x01 ENQ(EX) for `EVAC$WORKLOAD` to OVMXE (`ev2-mixed-refusal.pcap`
   record **3907**).
3. OVMXE sends **nothing back** and logs two lines:
   ```
   %DLM, a VMS system asked this node, its lock directory, for a resource this
         node itself holds locks on and does not master: not answered
   %DLM, refusing a lock message from a system that has not proved it runs this
         implementation
   ```
4. VAX1's `$ENQW(EX)` hangs forever. At T+75 s the OVMX standby converts NL→EX
   and is granted unopposed.

EX is compatible with the NL OVMX held, and OVMX *was* the resource's master, so
the faithful answer was a **grant** (Davis p. 6-31 outcome (a), p. 6-51).

## The cause, and it is not in the frame

From `ev2-OVMXE.console.log`, at the conversational boot:

```
SYSBOOT> SET LOCKDIRWT 1
%SYSGEN-E-NOSUCHP, no such parameter "LOCKDIRWT"
```

SYSBOOT's `SET` looked the name up only in the parameter **file** it had loaded,
and the shipped `SYS$SYSTEM:OVMXVMSSYS.PAR` seed was authored before LOCKDIRWT
existed (31 rows; `%SYSGEN-I-WRITTEN, 31 parameters written`). So:

- OVMXE joined at **LOCKDIRWT 0**;
- all three members at 0 ⇒ p. 6-32's all-zero rule ⇒ **one vector entry per
  system** (3 entries, two of them other systems');
- `vms_ldwv_sole_directory()` read **FALSE**;
- every arm of the interim mixed-cluster DLM is gated behind that predicate, so
  the directory role could not serve as master, `dlm_arm_serve_mixed_held()`
  declined, and the frame fell through to RULE C.

The first console line was additionally **false**: `vms_lock_dlm_name_mastered_here()`
was true on that node. Both lines together named the wrong fault, and the lab
spent a full run inferring the real one.

`ev3/` (a second arm at `04:37`, same build) showed the mirror image: VAX1 locked
first, OVMXE answered its lookup "you master it", and then an OVMX `$ENQ` was
granted EX locally while VAX1 still held EX — two holders. Same root cause, same
predicate.

## What the frame says (shipping decoders)

`tools/cluster/dlm_body.py --all ev2-mixed-refusal.pcap`:

```
f3907 req cat=02 op=01 ENQ  VAX1->52540000e501 mode=0x05(EX)
      reqid/pid=0x2020021e master_lkid=0x1a00021d reslen=13 res='EVAC$WORKLOAD'
```

plus `body[44:46]` = UIC group **1**, `body[46]` = access mode **3 (user)**, and
`body[128:132]` = `0x00027e10`, the directory hash **VAX1 itself** put on the wire
for that name (Davis p. 6-50) — the only value OVMX may ever assert for it.

The frame is committed verbatim as the host/sim fixture
`tests/cluster/host/fixtures/dlm-evac-workload-enq-from-vax1.spec`
(`origin: capture`, sha256 over the assembled 204 bytes), and
`tests/cluster/host/test_dlm_mixed_master.c` §1b drives it through the shipping
parser and the shipping master-side door: **unserved** on this bed, **GRANTED at
EX** once LOCKDIRWT really reaches the executive.

## Files

| file | what |
|------|------|
| `ev2-mixed-refusal.pcap` | all `ether proto 0x6007` on the pod bridge, 04:27–04:32Z (sha256 in `docs/clean-room/reference-captures.sha256`) |
| `ev2-OVMXE.console.log` | OVMXE's console, including the SYSBOOT NOSUCHP and both `%DLM` lines |
| `ev2-vax1.log`, `ev2-vax2.log` | the two real VAXes' consoles |
| `ev2-timeline.txt` | the run's own timestamped log |

## What the lab must show after the fix

1. `SYSBOOT> SET LOCKDIRWT 1` answers
   `%SYSGEN-I-SETPARAM, LOCKDIRWT changed from 0 to 1`, and `WRITE` reports
   **40** parameters.
2. At the state transition, OPA0: carries
   `%CNXMAN, lock directory weight vector built: every entry is this node's --
   this node is the SOLE lock directory node of the cluster`.
3. `CNXTRACE` prints `soledir=1`.
4. VAX1's `$ENQW(EX)` on `EVAC$WORKLOAD` is **granted** while OVMX holds NL, and
   the resource has exactly one master in `SDA> SHOW LOCKS` on both nodes.
5. If anything is still refused, the console now names which cause:
   `... this node is NOT the sole lock directory node ... check LOCKDIRWT on
   every member` versus `... this node IS the sole lock directory node, and this
   message is not one with a grounded mixed-cluster answer`.
