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
existed (31 rows; `%SYSGEN-I-WRITTEN, 31 parameters written`). So OVMXE joined at
**LOCKDIRWT 0** — all three members at 0 — and the build running at the time
(main, incl. #1568) confined its whole mixed-cluster lock arm to the *interim
sole-directory configuration* (`vms_ldwv_sole_directory()`: every weight-vector
entry this node's, which needs this node above 0 and every VMS member at 0). That
predicate read **FALSE**, so:

- the directory role could not serve as master,
- `dlm_arm_serve_mixed_held()` declined,
- and VAX1's frame fell through to RULE C and was refused.

The first console line was additionally **false**: `vms_lock_dlm_name_mastered_here()`
was true on that node. Both lines together named the wrong fault, and the lab
spent a full run inferring the real one.

`ev3/` (a second arm at `04:37`, same build) showed the mirror image: VAX1 locked
first, OVMXE answered its lookup "you master it", and then an OVMX `$ENQ` was
granted EX locally while VAX1 still held EX — two holders. Same single cause.

## What the frame says (shipping decoders)

`tools/cluster/dlm_body.py --all ev2-mixed-refusal.pcap`:

```
f3907 req cat=02 op=01 ENQ  VAX1->52540000e501 mode=0x05(EX)
      reqid/pid=0x2020021e master_lkid=0x1a00021d reslen=13 res='EVAC$WORKLOAD'
```

plus `body[44:46]` = UIC group **1**, `body[46]` = access mode **3 (user)**, and
`body[128:132]` = `0x00027e10`, the directory hash **VAX1 itself** put on the wire
for that name (Davis p. 6-50).

The frame is committed verbatim as the host fixture
`tests/cluster/host/fixtures/dlm-evac-workload-enq-from-vax1.spec`
(`origin: capture`, sha256 over the assembled 204 bytes).

## Both arms, on this bed, in `test_dlm_mixed_master` §1b

`rd vms-b5b0` retired the sole-directory confinement and routes every root name
through the **proven** resource-name hash, so the fix for both arms is the one
already in that change — and §1b proves it **on this bed**, with **this frame**:

| arm | assertion |
|-----|-----------|
| the bed | three members, every LOCKDIRWT 0, vector = one entry per system; the vector directs `EVAC$WORKLOAD` (group 1, user mode) **at this node** — which is where the real VAX really addressed its lookup, so OVMX's vector and the VAX's agree |
| **A** | OVMX holds NL and masters it; VAX1's **captured** ENQ(EX) is **GRANTED**, at EX, with VAX1's own requester handle and a master handle this executive minted. The lab got silence. |
| **B** | VAX1's **captured** frame arrives first and is answered "you master it" and recorded; the OVMX `$ENQ` then leaves as **one** op-0x01 addressed at VAX1, carrying the **learned** `0x00027e10` and the identity it is a value of, and **nothing above NL is granted here**. The lab's second holder cannot happen. |

### The held-out confirmation of the hash

`EVAC$WORKLOAD` appears in **no** corpus file in this tree — not
`dlm_hash_derivation.tsv`, not `dlm_hash_heldout.tsv`, not
`dlm_hash_predicted_m3soledir.tsv`, not `dlm_hash_prestudy_names.tsv`. The proven
function nevertheless computes **exactly** `0x00027e10` for (group 1, user mode,
`EVAC$WORKLOAD`) — the value a real OpenVMS VAX V7.3 put on this wire. That is an
independent, held-out confirmation off a real VMS node, asserted in §1b.

## Files

| file | what |
|------|------|
| `ev2-mixed-refusal.pcap` | all `ether proto 0x6007` on the pod bridge, 04:27–04:32Z (sha256 in `docs/clean-room/reference-captures.sha256`) |
| `ev2-OVMXE.console.log` | OVMXE's console, including the SYSBOOT NOSUCHP and both `%DLM` lines |
| `ev2-vax1.log`, `ev2-vax2.log` | the two real VAXes' consoles |
| `ev2-timeline.txt` | the run's own timestamped log |

## What the lab must show on the next fire

1. `SYSBOOT> SET LOCKDIRWT n` answers
   `%SYSGEN-I-SETPARAM, LOCKDIRWT changed from 0 to n` — **not** NOSUCHP — and
   `WRITE` reports **40** parameters. (LOCKDIRWT is no longer *required* for the
   mixed path; it is required for the operator to be able to configure the
   directory distribution at all, and seven other boot-read parameters were
   unsettable with it.)
2. At each state transition OPA0: carries
   `%DLM, lock directory weight vector: N entries over M systems, K of them this
   node's; weights advertised; a member does not run this implementation`.
3. `CNXTRACE` prints `dirvec_own=K`.
4. **Arm A:** with every member at the default LOCKDIRWT, an OVMX process takes
   NL on `EVAC$WORKLOAD` and a real VAX's `$ENQW(EX)` on it is **granted**;
   `SDA> SHOW LOCKS` on both nodes shows exactly one master.
5. **Arm B:** the VAX takes EX first and the OVMX `$ENQ` **waits** — no second
   holder, and `SDA> SHOW RESOURCE` on OVMX names the VAX as the master.
6. If anything is still refused, the console now names which of the three causes:
   an unanswerable message shape, a frame stating no resource identity, or a
   resource this node neither masters nor holds a directory entry for (that last
   one says "check that every member agrees on LOCKDIRWT and on the resource's
   directory node").
7. `vms_lock_dlm_dir_hash_computed_wrong()` reads **0** — no frame ever carried a
   different value for an identity this node had computed one for.
