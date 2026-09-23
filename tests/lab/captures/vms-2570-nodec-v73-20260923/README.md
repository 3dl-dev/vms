# vms-2570 — Node C V7.3 demo disk build + in-browser proof (2026-09-23)

rd vms-2570 (child of vms-735), decision rd vms-d24. See `tools/lab-vax/build_nodeC_vms73_cluster.sh`
and `tools/lab-vax/PROVENANCE-vms73-nodeC.md`.

## Disk

`vms73-nodeC-cluster.dsk.gz` — SHA-256 in `vms73-nodeC-cluster.dsk.gz.sha256`
(`45355fd2631c59666b0fe2839d17ed743e7d045646d2c179a8b3402915b4fe15`, 29,807,975 bytes). Built
from `/lab/media/openvms073.iso` + `/lab/cluster/data/d0-newinst073.dsk` inside `vaxlab-4`
(ovmx-lab k3s namespace), in an isolated `/lab/cluster-demo-nodeC-v73/` working directory — the
lab's live group-1 reference cluster (`/lab/cluster/data/d0.dsk`) was never touched. Uncompressed
disk SHA-256 recorded in `disk-sha256-precompress.txt`.

Identity (matches `tools/cluster-web-demo/mk_democonfig.py` `DEMO_NODE_C` exactly):
`SCSNODE=VAXC`, `SCSSYSTEMID=1989`, cluster group `257`, `VOTES=1`, `EXPECTED_VOTES=1`,
`ALLOCLASS=0`. Configured via the real VMS install dialog + `@SYS$MANAGER:CLUSTER_CONFIG_LAN`
(full transcript: `build-console.log`), never hand-injected bytes.

## Genesis proof (standalone, isolated SIMH inside vaxlab-4)

`build-console.log` shows the real sequence:
```
%CNXMAN,  proposing formation of a VAXcluster
%CNXMAN,  now a VAXcluster member -- system VAXC
%CNXMAN,  completing VAXcluster state transition
```
followed by an interactive SDA `SHOW CLUSTER` (`ANALYZE/SYSTEM`) confirming a solo CSB:
```
Address   Node    CSID      Votes   State     Status
83A42740  VAXC    00010001    1     local   member,qf_same,qf_noaccess
...
Curr. coord. CSID        00010001
```
and `F$GETSYI("EXPECTED_VOTES"),F$GETSYI("VOTES"),F$GETSYI("NODENAME")` = `1`, `1`, `VAXC`.

## pcjs bootability — CONFIRMED, not a blocker

The packaged `.dsk.gz` boots correctly under pcjs's KA655 browser machine
(`machines/dec/vax/browser/ovmx-cluster.html`, tested against a fresh `~/projects/pcjs`
worktree, commit `8da562328`). No pcjs-side blocker.

## In-browser proof: OVMX Node A (current main, `c8129e08` — carries vms-147/vms-0f8) + Node C

Setup: `tools/cluster-web-demo/build-cluster-demo` bundle built from the `build-boot-artifacts`
GitHub Actions artifact at SHA `c8129e08` (post vms-147 multicast fix + vms-0f8 discovery fix),
Node A config-injected to `SCSNODE=OVMXA`/`1987`/group `257` via the frozen injectors, served
locally (`demo/cluster/e2e/coi-server.js`) alongside a locally-served pcjs machine (this V7.3
disk). Run inside a temporary k3s-worker pod (`ovmx-lab/vms-2570-e2e`, deleted after use), never
served from vax.3dl.network.

**`e2e-run1-clean-transport.log`** (the existing `demo/cluster/e2e/e2e-boot.js` harness, unmodified,
NODE_C=this disk) — clean run, no crash: both nodes reach a running DCL/console state, real
bidirectional SCA (0x6007) traffic climbs continuously and symmetrically for the whole ~280s
observed window (`sca_byPort` VAXC 3→704, OVMXA 0→849 by t=282s; `nicTx` climbing in lockstep),
0 badclass/nobuf implied (no error counters fired). Node A reaches `%CNXMAN, waiting to form or
join an OpenVMS Cluster`; VAXC reaches `%CNXMAN, completing VAXcluster state transition` (its own
genesis) at t=54s. **Membership admission (`SHOW CLUSTER` naming both nodes) was NOT observed
within this window** — the circuit stayed open and busy but `pass`'s `acpOk` gate for VAXC never
fired (see "Known gap" below; diagnostic-only, does not affect this harness's actual `pass`,
which grades Node A's own SCA+ACP, not Node C's).

**`probe-run3-vaxc-login-sda.log`** + **`vaxc-fulldump-run3.txt`** — a one-off, throwaway probe
script (not added to the repo) that logs into VAXC (`SYSTEM`/`OVMXCLUSTER1`) and runs SDA's
`SHOW CLUSTER` directly (plain `SHOW CLUSTER` needs a terminal-table entry this minimally-
tailored disk doesn't carry — `%SMG-F-UNDTERNOS`, matched the interactive build session).
Taken ~15s after Node A's own CNXMAN first appeared: VAXC's CSB list still shows **`Nodes 1`**
(itself only) — i.e. **CN=2 had not converged that early**; consistent with run1's frame counts
still climbing at that point.

**`probe-run4-nodeA-kernel-panic.log`** and **`probe-run5-nodeA-boot-stall.log`** — two follow-up
attempts to let the circuit run longer before re-checking VAXC's SDA `SHOW CLUSTER`. Both were
confounded by **real host contention** on k3s-worker (a concurrent, unrelated pod — `cn2-stage1`,
another in-flight lane's warm pod — was actively consuming ~0.9–1.5 CPU at the same time; the
node's CPU **requests** were measured at 99% (7995m/8000m) during these runs, `kubectl describe
node k3s-worker`): run4's Node A guest kernel hit a real `Kernel panic - not syncing: IO-APIC +
timer doesn't work!` at t=12s (a known class of wasm/TCG-under-contention timing failure, not a
Node C or genesis-code issue); run5's Node A guest never printed anything past "Booting from
ROM..." for 180+s (starved, not crashed). Both rounds correctly report `Nodes 1` (Node A never
actually reached a state where it could join).

## Honest status — CN=2 (Node A + Node C V7.3) in-browser: NOT YET PROVEN

What IS proven: the V7.3 disk is real, byte-correct, forms genesis both standalone and in-browser,
and the transport carries real, sustained, bidirectional, error-free 0x6007 traffic between OVMX
Node A and it (same mechanism already proven symmetric-OVMX-to-OVMX and OVMX-to-real-VMS-5.5
elsewhere in this epic). What is NOT yet proven: full membership convergence (`SHOW CLUSTER`
naming both nodes on both sides) within an observed window — the one clean, uncontended run
(e2e-run1) was not watched past t=282s, and the follow-up attempts to watch longer were blocked
by real k3s-worker CPU contention with a concurrent lane, not by anything in this disk or in
Node A's own fixes. This is reported as an **open gap**, not fabricated as done.

## Known gap (diagnostic only, does not block this item)

`~/projects/pcjs` `ovmx-cluster.html`'s real-VMS authenticity tell
(`ACP_TELL_VMS_BANNER = /VAX\/VMS\s+Version\s+V\d+\.\d+-\w+/i`) was tuned for V5.5's exact banner
text ("VAX/VMS Version V5.5-2H4") and does not match V7.3's actual banner ("Welcome to OpenVMS
(TM) VAX Operating System, Version V7.3") — so the page's own `acp-ok` UI tell / `bar()` message
never fires for a V7.3 Node C, though this does not affect `e2e-boot.js`'s actual `pass` gate
(which only grades Node A). Filed as a follow-on, not fixed here (out of this item's scope —
pcjs page authenticity-tell logic, not the disk or the OVMX executive).

## Retry recommendation

Re-run the longer-duration in-browser CN=2 watch (`e2e-boot.js` with `NODE_C=` this disk,
`DEADLINE_MS` >= 600000) on an UNCONTENDED k3s-worker window (check `kubectl top pods -n
ovmx-lab` first) to determine whether membership converges given enough real time, independent
of the contention that blocked runs 4–5 here.
