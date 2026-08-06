# Lab-2 — VMScluster reference labs on k3s (`vms-a5c`)

Parallel capacity for the cluster-interop RE work. **Lab-1** — the SIMH VAX
cluster at `/data/training/vax/cluster`, run by hand on workshop — is a single
instance, and `vms-2f3`'s live rejoin investigation owns it. That serialises
every other `vms-ci` item behind one investigation. This spins up additional,
fully isolated labs on the k3s cluster **without touching lab-1**.

Clean-room invariant (project Rule 8) is unchanged: these labs are *observation*
oracles. Wire formats come from watching the wire and from public OpenVMS
documentation. Never disassemble or decompile VSI/HPE material.

## The design in one paragraph

**One pod == one complete lab.** Each StatefulSet replica clones its own copies
of the golden disk images and builds its own `br0` + `tapN` **inside its own pod
network namespace**. The 0x6007 LAVC/SCA HELLO multicast therefore never reaches
the cluster LAN, and replicas cannot see each other — so every replica reuses the
same SCSSYSTEMIDs (1025/1026/1027) and node MACs by design. Scale the
StatefulSet to get more labs.

## Why no cluster plumbing was needed

`mk-k3s-worker.sh` already attaches the tank `training` virtiofs share to
`k3s-worker`, and the lab dataset `tank/vax` is nested *inside* it. So
`/data/training/vax` is **already mounted on the node** — verified 2026-08-02
with a read-only probe pod (`vax1`, `vax2`, `vax-n`, templates, `clean-cluster`,
`captures` all visible; 37.7 G free of the 40 G quota). No new ZFS dataset, no
`pvesh` mapping, no worker rebuild.

> ⚠️ **Do not run `mainframe/scripts/mk-k3s-worker.sh` to "set this up".** It
> destroys and recreates VM 214, and nine bound `local-path` PVCs belonging to
> the enterprise-ai stack live on that node's disk. Tracked as `mainframe-769`.

## Deploy

```bash
# 1. build + push the node image (from a machine with docker; workshop has it)
docker build -t 192.168.2.43:30500/ovmx-vaxlab:1 tests/lab
docker push  192.168.2.43:30500/ovmx-vaxlab:1

# 2. apply — this starts ONE lab
kubectl apply -f tests/lab/k8s/

# 3. watch it boot (~90 s to VMS + cluster formation)
kubectl -n ovmx-lab logs -f vaxlab-0

# 4. more labs
kubectl -n ovmx-lab scale sts/vaxlab --replicas=3
```

## Drive a lab

The console interface is lab-1's exactly — `nodedrv.py` under a pty, an input
FIFO per node, console output tee'd to a log on the volume.

```bash
K="kubectl -n ovmx-lab exec vaxlab-0 --"
L=/lab/k8s-labs/vaxlab-0/logs

$K sh -c "printf 'SHOW CLUSTER\r' > $L/vax1.log.in"
$K tail -40 $L/vax1.log
$K sh -c "tcpdump -i br0 -w $L/formation.pcap -U -s 0 &"     # capture inside the pod
```

**Login is prompt-synchronised** — wait for `Username:`, send `SYSTEM`, wait for
`Password:`, send `system`. Batched sends race the boot chatter and fail as
`%LOGIN-F-INVPWD`, which is *never* actually a bad password. Then
`SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST`.

Captures and logs land on the tank volume under `k8s-labs/<pod>/logs/`, readable
from workshop at `/data/training/vax/k8s-labs/`.

## Verification (2026-08-02, image `:2`)

`vaxlab-0` boots vax1 + vax2 to `SHOW CLUSTER` MEMBER/MEMBER under the x86_64
SIMH build. A 130 s steady-state capture on the pod's `br0`, decoded with
`docs/clean-room/tools/af2scan.py`, was compared against lab-1's 2-node baseline
`cluster/captures/cd0-baseline-current-20260728.pcap`:

| signature | lab-1 baseline | lab-2 (`vaxlab-0`) | |
|---|---|---|---|
| HELLO multicast dst | `ab0004010101` | `ab0004010101` | same — cluster group 1 |
| node MAC | `aa0004000104` | `aa0004000104` | same — carried by the disk image |
| peer MAC | `08002b7856b9` | `08002b8eb785` | differs; both DEC OUI, SIMH per-instance |
| `0xa0` HELLO rate | 0.98/s | 0.89/s | ≈1/s aggregate for 2 nodes |
| `0xb3`/`0xb4` | 8 / 8 | 57 / 57 | exactly paired in both |
| type vocabulary | — | subset of lab-1's | no type appears that lab-1 never emits |

The `0x4b` rate differs (0.63/s vs 1.47/s) but the baseline sample is 9 frames
over 14 s — too small to read anything into. Everything with a usable sample
agrees.

**What this does and does not establish.** The lab-2 wire carries the same
message vocabulary, the same multicast and node addressing, and the same HELLO
cadence and `0xb3`/`0xb4` pairing as the reference lab. It is **not** a
byte-level fidelity proof — no field-level diff was run. The arch-difference
caveat below still governs: reproduce a *contradicting* lab-2 result on lab-1
before trusting it.

## Check the pod is a CLUSTER before you use it (`vms-70e2`, 2026-08-05)

A replica can be `1/1 Running` for days with its two VAXes not clustered to each
other. `vaxlab-0` and `vaxlab-1` were both in that state on 2026-08-05 — vax1 and
vax2 each reporting `CLUSTER_NODES = 1`. A join test against such a pod fails for
reasons that have nothing to do with the daemon under test.

```bash
K="kubectl -n ovmx-lab exec vaxlab-N --"; L=/lab/k8s-labs/vaxlab-N/logs
$K sh -c "printf 'WRITE SYS\$OUTPUT \"CN_\"+F\$STRING(F\$GETSYI(\"CLUSTER_NODES\"))\r' > $L/vax1.log.in"
# CN_2 = a healthy 2-node lab.  CN_1 = broken, do not run an experiment on it.
# CN_3 = an OVMX node is currently joined.
```

Scaling up is cheaper than repairing one: `kubectl -n ovmx-lab scale sts/vaxlab
--replicas=N` gave a healthy virgin pod in ~2.5 minutes. The StatefulSet was left
at **6 replicas**; `vaxlab-4` carries the `vms-70e2` bracket.

## Mint every identity through `mk_sysgen.py` — never by hand (`vms-1ae`)

`SCSNODE` and `SCSSYSTEMID` are cluster-wide unique keys. Present either one
that the peer's configuration poller has *recently seen on another system* and
the join is refused outright:

```
%PEA0, Remote System Conflicts with Known System - REMOTE NODE OVMXxx
```

The VC opens, the peer's connect never comes, `CLUSTER_NODES` never moves —
i.e. it looks **exactly** like the `vms-2f3` stall, and every run under a
collided identity is a null result filed against the wrong cause. Bracketed
live over 13 arms on three pods; the rule, the aging behaviour, and the fact
that an *exact* rejoin does **not** trip it are in
`docs/cluster-protocol-spec.md` §4(w).

This has already bitten: parallel agents minted `OVMXY1` and `OVMXP1` both on
`SCSSYSTEMID` 1601, and `OVMXP2`/`OVMXY2` both on 1602. `mk_sysgen.py` now
refuses a colliding mint and names the store it collides with; ask it for a
free pair rather than guessing one:

```bash
python3 tests/lab/tools/mk_sysgen.py --alloc OVMXR /data/training/vax/cluster/work
# -> OVMXR0 1812
```

`--force` overrides, loudly, for the arms where the collision *is* the
experiment. `tests/lab/tools/conflictbracket.sh` is the runner those arms use:
identity is an argument, the console is windowed to the run (vax1.log is
append-only for the life of the pod, so a bare grep finds someone else's
message from hours ago), and both consoles are read.

## Things that will bite you

- **`dep bdr 0` stays commented on every node.** Depositing 0 into the KA655
  Boot/Diagnostic Register forces unattended autoboot: the node never drops to
  `>>>` and boots whatever root `nvram.bin` defaults to — which is how vax2 came
  up as a second "VAX1" on 2026-07-28 (`vms-d0f`). The console driver picks each
  node's root via `B/R5:...` instead.
- **`set idle=vms` is load-bearing.** Without it SIMH spins a core forever on the
  VMS idle loop and every replica pegs its CPU limit doing nothing.
- **Never point two labs at one `d0.dsk`.** Each replica clones its own; within a
  lab, vax1 and vax2 share it deliberately (dual-ported, identity = system root).
- **Different binary from lab-1.** Lab-1's `./vax` is an aarch64 build from the
  old Surface seat; this image builds the same open-simh commit (`2e0d51e`) for
  the pod's arch. Before trusting a lab-2 result that **contradicts** a lab-1
  result, reproduce it on lab-1. Agreeing results need no such check.
- **Disk space is shared.** The 40 G tank quota covers lab-1 *and* every replica.
  Clones are sparse (~120 MB real per lab against 1.5 G apparent), but
  `df -h /data/training/vax` before scaling far.

---

<!-- vms-578: worktree-760 wrote a DIFFERENT README at this path -- a guide to
     `tests/lab/tools/`. Both are kept: the lab-2 capacity story above, then the
     tooling snapshot below. Neither replaced the other. -->

## `tests/lab/` — VMScluster interop lab tooling

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
| `portwatch.sh` | `csbwatch.sh` with SDA `SHOW CONNECTIONS/NODE=` — the peer's CDT table and its `Rej/Disconn Reason`. |
| `portrun.sh` | `lab2run.sh` with **both** VAX consoles bracketed by byte offset. Use it whenever the claim is "the peer said X": the console logs are append-only and shared with every earlier run on that pod, so a bare `grep` finds somebody else's scrollback and reads as a reproduction (`vms-0fe`). |

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
