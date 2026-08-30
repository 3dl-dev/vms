# `capture_oracle` — byte-exact golden capture from the live OpenVMS labs (vms-55d)

Capture the **display output a user actually sees** for a named surface from a
**live OpenVMS lab oracle** (lab-2 VAX V7.3, lab-Alpha V8.4), as a versioned
byte-exact golden. This is the reusable, trap-respecting productization of the
console-driving the cluster-RE and UX-fidelity work does by hand; the goldens
feed the `vms-050` UX-fidelity golden-comparison gates (`vms-c38` / `vms-d008`).

Capturing observable tool output on the real system is the sanctioned clean-room
grounding (CLAUDE.md **Rule 8** — observation, never disassembly, never VSI
source).

## Use

```sh
tools/oracle/capture_oracle.sh list                     # known surfaces
tools/oracle/capture_oracle.sh selftest                 # slice logic, no lab
tools/oracle/capture_oracle.sh <surface>                # capture -> golden
tools/oracle/capture_oracle.sh <surface> --keep-lab     # leave the replica up
```

A run writes:
- `docs/oracle/golden/<surface>.golden` — the byte-exact display (leading spaces
  and column positions preserved; this is what a gate diffs OVMX output against).
- `docs/oracle/golden/<surface>.golden.meta` — provenance (arch, oracle node,
  capture timestamp, the exact commands), kept out of the `.golden` so the golden
  stays pure bytes.

## Surfaces

A **surface** is a small sourced data file, `tools/oracle/surfaces/<name>.surface`:

```sh
ARCH=vax            # vax | alpha
DESC="one line"
COMMANDS=(
  'IDENT_L = %X80000004'
  'SHOW SYMBOL IDENT_L'
)
```

`capture_oracle` runs `COMMANDS` between unique markers and slices the transcript
byte-exactly (`extract_golden`, proven by `selftest`). Prefer **deterministic**
surfaces (fixed inputs) so the golden is reproducible; a surface with live
numbers (free pages, times) pins layout/labels, not the varying digits.

## Lab traps this respects (so you don't have to relearn them)

- **base64 end-to-end through the console FIFO** — DCL is full of `$` and `"`.
- **FIFO only, never a second console connection** — AXPbox (Alpha) *exits* when
  a console client disconnects; a TCP readiness probe powers it off.
- **a bare RETURN wakes OPA0:** — the boot parks at "SYSTEM job terminated"
  before the `Username:` prompt appears.
- **prompt-synchronised login, one line at a time** — batched sends race the boot
  chatter and fail as `%LOGIN-F-*` (never actually a bad password). VAX login is
  `SYSTEM`/`system`; Alpha is `SYSTEM`/`ovmxlab2026`.
- **never touch a busy/shared pod** — the tool always scales UP its *own*
  isolated replica (`cur+1`, the new highest-index pod) and scales back after
  (`--keep-lab` to skip); it never drives `vaxlab-0/1` or a pod it did not create.
- **read the console with a printable filter** (`tr -cd`), not `strings` (which
  intermittently drops the SRM/console control stream).

## Requirements

`kubectl` with access to the `ovmx-lab` namespace (override with `OVMX_LAB_NS`).
No Docker build — it drives the already-running lab StatefulSets, so it is
disk-safe. AXPbox/SIMH boot is ~1–2 min, so a capture takes a few minutes; run it
detached if your shell has a short command timeout.
