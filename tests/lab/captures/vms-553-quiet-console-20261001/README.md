# The OpenVMX/VAX console without the NetBSD boot, fixed at the source (rd vms-553, 2026-10-01)

Operator ruling, 2026-10-01: the OVMX/VAX console must not show NetBSD/unix boot output. The fix belongs in the kernel, not in a display filter on the web demo. Branch `work/vms-553-kernel-quiet` (#1347) is V0.7-5 plus the quiet substrate (`tools/cross-vax/netbsd-ovmx-quiet.patch`).

## What was run (k3s-worker, pod in ovmx-lab)

| check | result |
|---|---|
| `run-boot.sh kernel-quiet` (SIMH) | PASS. The raw console from `>>>B/R5:2 DUA0` to the single-user shell showed only the KA655 ROM/VMB lines (`2.. -DUA0 1..0..`). `dmesg` held the copyright, `total memory`, `avail memory`, `mainbus0`, `boot device` and `root on` lines. |
| `run-boot.sh sysboot-single`, Node B (OVMXB/1988, VOTES 0, EXPECTED_VOTES 1, group 257) | PASS. Username:, then SYSTEM/MANAGER to `$`. The new raw-console substrate check had no hits. |
| `run-boot.sh sysboot-single`, single-node pane | PASS, same checks |
| `test-console-raw-quiet.mjs` (headless Chromium, both disks, served locally and then live from vax.3dl.network) | PASS on all four runs. See `live-*-raw-console.log`. |
| `visitor-gate.mjs` RUNS=1 against the live https://openvmx.3dl.dev/demo/cluster/ | **CN=3** (`visitor-gate-*.{log,json}`). Node B's full console for that run is `live-cluster-OVMXB.console.log`: 62 lines with 32 `%CNXMAN` lines, and `substrate_lines()` finds nothing. |

## Deployed

| piece | sha256 |
|---|---|
| `ovmx-vax-nodeB.img.gz` (pcjs dd66331f7) | gz `0929fff796f8e589…`, raw `a86ff4de417ddfb5…` |
| `ovmx-vax-v0.7-5.img.gz` (pcjs dd66331f7; homepage repointed by openvmx-site#58) | gz `5cef0d7a081e5685…`, raw `97e6251335fbda9d…` |
| pcjs 92e010648 | removes the vaxterm.js `hideRange` and ovmx.html `updateHideRange` (rd vms-1e2), so both pages now show the console verbatim |

## Still on the console, and why

- **KA655 ROM and VMB output**: the self-test countdown, `>>>`, `(BOOT/R5:0 DUA0)`, `2.. -DUA0 1..0..`. This is the faithful VAX firmware. Real VMS shows the same lines.
- **The executive's operator lines** (`%PEA0, ...`, `%CNXMAN, ...`, `%MSCP_CL`, `%DLM`). These are the VMS personality, sent via `TOCONSOP` without the kernel timestamp. The log copy keeps the timestamp.
- **By design, after a failure only**: a kernel panic and everything after it; a verbose boot (`B/R5:20000`); RB_ASKNAME/RB_USERCONF; the kernel asking for a root device; a `/boot` autoboot that fails, or a key pressed during its countdown.
