# `tests/lab/` — VMScluster interop lab tooling

Snapshot of the lab harness used to reverse-engineer the VMScluster wire
protocol against a real 2- or 3-node SIMH VAX/VMS 7.3 cluster.

**Why this directory exists.** The live tooling lives at
`/data/training/vax/cluster/tools/` on the `workshop` dev host, which is **not a
git repository**. That has already cost the project once: `mk_sysgen`'s source
was lost and had to be reimplemented from scratch as `mk_sysgen.py`
(`docs/HANDOFF-vms-2f3.md` §4e.1). Tracked as `vms-f7a`.

**This is a snapshot, not the working copy.** The scripts carry absolute paths
to the lab volume and the pod namespace and are not runnable from a checkout as
they stand. They are preserved here so the *method* survives the host. Re-sync
deliberately; do not assume this copy is current.

## What each tool is for

| script | what it does |
|---|---|
| `csbwatch.sh` | One OVMX attempt on a lab-2 pod with the peer's **CSB** for our identity sampled *through* it. The instrument behind §4L and §4M. |
| `connwatch.sh` | Same runner with SDA `SHOW CONNECTIONS/NODE=` instead — the peer's **CDT** table and its `Rej/Disconn Reason`, the only oracle in the lab that names a *rejection* rather than describing a silence. |
| `connpoll.sh` | The lab-1 (non-pod) ancestor of `connwatch.sh`. |
| `csbcycle.sh` | SIGKILL a **real** node and sample the peer across kill → removal → reboot → readmission. Produces the real-node control. |
| `scacptrace.sh` | High-cadence SCACP (`SHOW VC`) — PEDRIVER's own view, plus packet capture. Established event *ordering*. |
| `scacppoll.sh` | Lower-cadence SCACP predecessor; no capture. |
| `stallpoll.sh` | One join with SDA polled on a **chosen** node during the stall. The node is an argument on purpose — see §4d.6. |
| `lab2run.sh` / `oneshot.sh` | Plain runs, no peer-side instrumentation. |
| `cycle2.sh` | Join/exit cycling with a per-cycle identity and per-cycle env. |
| `bootlab.sh` | Boot the lab from live disks with no golden restore. |
| `mk_sysgen.py` | Mint a SYSGEN store for a new `SCSNODE`/`SCSSYSTEMID`. Patches a known-good template rather than regenerating, so every other field matches a store proven to join. Rejects names >6 chars (VMS truncates) and deletes any stale `.cluster` sidecar. |
| `seqchk.py` | Capture-integrity check: sequence gaps and truncated frames. |
| `probe.sh` | Drive any VAX console and capture between markers. |

## Harness lessons baked into these scripts

Each was paid for with a wasted or wrong run; the script headers carry the
detail.

1. **Stay inside SDA/SCACP.** Re-entering the utility per sample overruns the
   console input buffer during an OPCOM flood and silently swallows samples.
2. **Narrow the query** (`/NODE=`). A bare `SHOW CONNECTIONS` is ~230 lines per
   sample, which makes repeated sampling unsafe.
3. **Derive length words from what you emit, never inherit them** — an
   over-declared frame is dropped as a runt, in silence, because there is no NAK
   anywhere in this protocol.
4. **Prove which identity a control controlled for**, on the wire:
   `strings -a work/d94-<tag>.pcap | grep -oE 'OVMX[A-Z0-9]{2}'`.
5. **Bracket a negative run with a positive control on both sides**, not just
   before it.
6. **Sample while the process is alive.** A peer-side sample taken after our
   process exits measures our corpse, not our attempt.
7. **Read the other node's console.** Tools that park VAX1 in SDA leave VAX2's
   console free, and VMS prints the entire membership dialogue to OPCOM there.

Full guardrail list: `docs/HANDOFF-vms-2f3.md` §7.
