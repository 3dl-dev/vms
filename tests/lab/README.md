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
