# Lab-Alpha — a 64-bit OpenVMS oracle on k3s (`vms-e2c`)

## Why this exists

Lab-1 (`/data/training/vax/cluster`) and lab-2 (`tests/lab/`) are both **OpenVMS
VAX V7.3**. VAX is a 32-bit architecture. **No VAX node can answer a question
about 64-bit behaviour** — not because OpenVMS is 32-bit, but because the
hardware under those particular nodes is.

Measuring OVMX against a VAX and concluding "VMS does X" reads an *architecture*
limit as an *OS* rule. This node makes the 64-bit answer measurable instead of
inferred. Clean-room invariant (Rule 8) is unchanged: it is an **observation
oracle**, nothing is disassembled or decompiled.

## Status — OpenVMS Alpha V8.4 is installed and running

| Piece | State |
|---|---|
| Emulator — AXPbox v1.2.0, AlphaServer ES40 / EV68CB | working |
| SRM console firmware V7.3-1 | working |
| **OpenVMS Alpha V8.4** on a 9 G ODS-5 disk | **installed, boots, logs in** |
| Golden reference image | **snapped + verified** |
| Unattended boot from golden (clone → login prompt, no typing) | working |
| Runs on k3s, not the dev seat | working |
| Networking (DE500 over veth+bridge) | working |
| **Two-node Alpha VMScluster** | **forms**, then hits AXPbox bug #83 — see below |

Node identity: `SCSNODE=ALPHA1`, `SCSSYSTEMID=2049` (DECnet 2.1), TZ GMT,
ODS-5 with hard links, `SYSTEM` password `ovmxlab2026`.

Installed: OpenVMS 8.4, TCP/IP Services 5.7-13, CDSA, Kerberos, SSL, TDC_RT,
Availability Manager, Binary Checker. **Not** installed: DECwindows Motif,
DECnet (either flavour) — see "Why no DECnet".

### The measurement that motivated the node

`vms-c71` changed OVMX's `SHOW SYMBOL` to render integers as a longword, on the
strength of a VAX-only oracle. Asked of **real OpenVMS Alpha V8.4**:

```
$ IDENT_L = -2147483644
$ SHOW SYMBOL IDENT_L
  IDENT_L = -2147483644   Hex = 80000004  Octal = 20000000004
$ IDENT_D = 8388736
$ SHOW SYMBOL IDENT_D
  IDENT_D = 8388736   Hex = 00800080  Octal = 00040000200
```

Byte-identical to the VAX 7.3 oracle. And DCL *arithmetic* is a longword on
Alpha too:

```
$ BIG = 2147483647 + 1
$ SHOW SYMBOL BIG
  BIG = -2147483648   Hex = 80000000  Octal = 20000000000
$ HUGE = 4294967300
$ SHOW SYMBOL HUGE
  HUGE = 4   Hex = 00000004  Octal = 00000000004
```

**So the `vms-c71` fix is correct on 64-bit hardware, not just on the VAX.** DCL
integer width is architecture-invariant at 32 bits. Nothing was "nuked". This is
now measured on both architectures rather than inferred from one — which is the
whole point of having the node, and is the shape every entry in `vms-580` should
take.

## Drive a lab — the agent protocol

**Read this before touching the lab. Do not transfer habits from lab-2** — the
console contract is different in ways that silently destroy a run.

### Layout

One pod is one lab. Each node inside it owns a directory:

```
/lab/k8s-labs/<pod>/<node>/logs/<node>.log      console output (tee'd)
/lab/k8s-labs/<pod>/<node>/logs/<node>.log.in   console input FIFO
/lab/k8s-labs/<pod>/<node>/disks/<node>-sys.img system disk
/lab/k8s-labs/<pod>/<node>/rom/                 NVRAM + TOY clock (per node)
```

Readable from workshop at `/data/training/vax/k8s-labs/<pod>/…`. Nodes are
`alpha1`, `alpha2`, … and console ports are `21264 + index`.

### Capacity

```sh
kubectl -n ovmx-lab get pods -l app=alphalab
kubectl -n ovmx-lab logs alphalab-0                    # bring-up + console
kubectl -n ovmx-lab scale sts/alphalab --replicas=3    # more labs
```

Each replica clones its own disks from the golden image and builds its own
bridge in its own netns, so replicas cannot see each other and all of them reuse
`ALPHA1`/2049 by design — the same trick lab-2 plays. **Scaling up is cheaper
than repairing a sick pod.** Watch the shared 40 G tank quota (`df -h
/data/training/vax`) and k3s-worker's CPU: one core *per emulated node*, always,
because AXPbox never idles.

### Sending a console line — always base64

```sh
POD=alphalab-0; NODE=alpha1
FIFO=/lab/k8s-labs/$POD/$NODE/logs/$NODE.log.in
B=$(printf '%s' 'SHOW SYSTEM' | base64 -w0)
kubectl -n ovmx-lab exec $POD -- sh -c "{ echo $B | base64 -d; echo; } > $FIFO"
```

**Never interpolate DCL into a shell command.** DCL is full of `$` and `"`, and
every shell between you and the FIFO will expand them. A command like
`WRITE SYS$OUTPUT F$GETSYI("ARCH_NAME")` arrives as `WRITE SYS F(ARCH_NAME)` and
DCL answers `%DCL-W-UNDSYM` — which reads like a VMS problem and is not one.
Base64 end-to-end is the only safe path. `tests/lab-alpha/tools/srmdrv.py` does
this correctly if you drive the console directly instead of through the FIFO.

### Logging in — prompt-synchronised, one step at a time

Wait for `Username:`, send `SYSTEM`; wait for `Password:`, send `ovmxlab2026`.
Batched or timed sends race the boot chatter and fail as `%LOGIN-F-CMDINPUT`,
which is **never** actually a bad password — it means LOGINOUT read input it did
not expect. (Lab-2's equivalent symptom is `%LOGIN-F-INVPWD`.) Then
`SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST`.

### Check the node is healthy before you use it as an oracle

A pod can be `1/1 Running` with its Alpha sitting at `P00>>>`, halted, mid-dump,
or crashed — none of which is an oracle. This is the Alpha analogue of lab-2's
`CLUSTER_NODES` check, and it is not optional.

```sh
D=/data/training/vax/k8s-labs/alphalab-0/alpha1/logs/alpha1.log
strings $D | tail -5
```

| what you see | verdict |
|---|---|
| `Username:` or a `$` prompt | healthy — usable |
| `P00>>>` | at SRM firmware, **VMS is not running** |
| `Bugcheck code =` / `Starting compressed selective memory dump` | crashed, do not use |
| `waiting to form or join an OpenVMS Cluster` | blocked on quorum, not usable |
| nothing / empty log | console pump never attached — see `logs/pump.log` |

**Recovering a node that is back at `P00>>>`** (a crash halts the machine, and
the unattended-boot helper only runs once at pod start, so nothing re-boots it
for you):

```sh
B=$(printf '%s' 'boot dqa0' | base64 -w0)
kubectl -n ovmx-lab exec alphalab-0 -- \
  sh -c "{ echo $B | base64 -d; echo; } > /lab/k8s-labs/alphalab-0/alpha1/logs/alpha1.log.in"
```

If the clock is cold it will then ask for the date — answer
`DD-MMM-YYYY HH:MM` in caps. Deleting the pod re-runs the whole unattended path
and is often quicker.

Confirm the machine really is the architecture you came for before you quote it:

```
$ WRITE SYS$OUTPUT F$GETSYI("ARCH_NAME")+" / "+F$GETSYI("HW_NAME")
Alpha / AlphaServer ES40
```

### Asking an oracle question properly

The point of this lab is a *second* architecture, so a single-architecture
answer is half a result. Ask the **same** question of a VAX (lab-1 or lab-2) and
of lab-Alpha, and record both — including when they agree. "No divergence,
measured on both" is worth as much as a divergence and costs one extra run; that
is the form `vms-580` wants. A divergence is a **result**, not a fault: reproduce
it behind an explicit architecture decision in `docs/architecture.md`, never
paper over it.

### Capturing the wire

```sh
kubectl -n ovmx-lab exec alphalab-0 -- \
  sh -c 'nohup tcpdump -i br0 -w /lab/k8s-labs/alphalab-0/logs/run.pcap -U -s 0 >/dev/null 2>&1 &'
```

`$LAB_DIR/logs/` is **not** created by the entrypoint — `mkdir -p` it first or
tcpdump exits silently and you get no capture (this cost a run). Cluster traffic
is ethertype `SCA (0x6007)`; HELLO multicast goes to `ab:00:04:01:ea:08`.

### Never run this on workshop

AXPbox has **no idle detection** — unlike SIMH's `set idle=vms`, an idle
emulated Alpha still spins a host core flat out. Run on the dev seat it drove
load average past 20 and destabilised the box, killing two agent sessions. The
bare-metal path (`setup.sh` / `node.sh`) still exists and still works, but the
pod is the supported way to run one; the pod's CPU limit is a hard ceiling and
the blast radius of a runaway emulator is one pod.

## Things that will bite you

Every one of these cost a real run. The first two have no lab-2 equivalent and
are the ones that catch agents arriving from `tests/lab/`.

1. **AXPbox exits when the console client disconnects.** A one-shot connect,
   command, disconnect powers the machine off — lab-2's SIMH nodes survive this,
   these do not. Hence the persistent pump; drive the node through its FIFO, and
   never open a second connection to the console port "just to check".
2. **A TCP readiness probe kills the machine.** The obvious "is the console
   port listening yet?" check *is* a client connecting and disconnecting. It
   powered the Alpha off a second before the real console attached; the symptom
   was an emulator that "died instantly" with an empty log. `srmdrv.py` retries
   the connect internally (`-C`) so the first connection ever made is the pump's.
3. **A CD-ROM must not share an IDE channel with the system disk.** With the ISO
   as slave on channel 0 (`disk0.1` → `DQA1`) the SRM reads block 0, calls it a
   valid boot block, and then fails the very next multi-block read. On the SCSI
   adapter the media reads fine but **OpenVMS 8.4 bugchecks `INVEXCEPTN` before
   the installer**. Only the CD as master on the *second* IDE channel
   (`disk1.0` → `DQB0`) actually installs. This one difference is what separated
   "cannot boot VMS at all" from a completed install.
4. **`flash.rom` / `dpr.rom` are NVRAM and the TOY clock — keep them.** Wiping
   them per boot loses `bootdef_dev` and leaves the clock cold, and VMS then
   stops at `Please enter date and time` instead of coming up. They are already
   per-pod, so there is nothing to gain by deleting them.
5. **The NIC needs a veth, never a tap** — the opposite of lab-2. Full
   explanation under "The networking trap" below; the symptom is `EWA0 Link
   state: UP` with literally zero packets on the wire.
6. **Interpolating DCL into a shell corrupts it.** Base64 end-to-end; see the
   agent protocol above.
7. **`$LAB_DIR/logs/` does not exist until you make it.** `tcpdump -w` into it
   fails silently and you finish the run with no capture.
8. **Disk space is shared with lab-1 and every lab-2 replica** — one 40 G tank
   quota. Alpha clones are sparse (~242 M real against 9 G apparent), but
   `df -h /data/training/vax` before scaling far. Note separately that
   workshop's `/` has repeatedly hit 100%, which breaks image builds.

## Which AXPbox build

| Version | Verdict |
|---|---|
| **v1.2.0** (Jul 2026) | **In use.** Reaches SRM in ~20 s and installs/boots OpenVMS 8.4 on the dual-channel IDE layout. Rejects the `icache` option the old guides set. |
| v1.1.2 (Apr 2024) | Unusable. The published release binary is a debug build — it pegs the CPU spewing `IP interrupt set/cleared` and never reaches the SRM prompt. |

Both are baked into the image, so `AXPBOX_VERSION` A/Bs them with a redeploy
rather than a rebuild. Each binary prints its own commit hash and `entrypoint.sh`
logs it, so every run states exactly which build produced it.

## Golden image

`/data/training/vax/alpha/disks/alpha1-sys.golden.img` — 9 G apparent, ~242 M
real (sparse + ZFS zstd), md5 in the adjacent `.md5` file. Snapped after a clean
`@SYS$SYSTEM:SHUTDOWN`, and verified by md5 against its source.

With `GOLDEN` set, a replica clones it and **autoboots to a login prompt with no
manual step** — `entrypoint.sh` answers both the SRM prompt and the cold-clock
date prompt. To build a *new* golden instead, unset `GOLDEN` and set
`MEDIA=ALPHA084.ISO`; the pod comes up at `P00>>>` with the CD on `DQB0`.

> Verifying a sparse copy: `du` right after `cp` can report ~1 K because ZFS has
> not flushed. `sync` first, and compare md5 — not size.

## The Alpha cluster — it forms, then hits an emulator bug

**A two-node Alpha VMScluster forms.** `NODES="alpha1 alpha2"` puts a whole
cluster in one pod, on a bridge in the pod's own netns:

```
%CNXMAN,  Proposing formation of a VMScluster
%CNXMAN,  Now a VMScluster member -- system ALPHA1
%CNXMAN,  Completing VMScluster state transition
```

with real SCA traffic on the wire, captured at
`/data/training/vax/alpha/captures/alpha-cluster-formation.pcap`:

```
08:00:2b:00:00:02 > 08:00:2b:00:00:01, ethertype SCA (0x6007), length 100
08:00:2b:00:00:01 > 08:00:2b:00:00:02, ethertype SCA (0x6007), length 124
08:00:2b:00:00:01 > ab:00:04:01:ea:08, ethertype SCA (0x6007), length 134
```

~300 unicast frames between the two nodes plus HELLO multicast to
`ab:00:04:01:ea:08`. **This is the first non-VAX SCS capture the project has** —
the same protocol `vms-2f3` studies, from a 64-bit implementation.

**Then ALPHA1 bugchecks, reproducibly, immediately after joining:**

```
%CNXMAN,  Now a VMScluster member -- system ALPHA1
**** OpenVMS Alpha Operating System V8.4     - BUGCHECK ****
** Bugcheck code = 0000036C: PROCGONE, Process not in system
** Current Process:       SYSINIT
```

ALPHA2 then reports `Lost connection to system ALPHA1`, removes it, and blocks
on lost quorum — correct behaviour on its part. `PROCGONE` 0x36C is AXPbox's
known issue **#83**. **Every join has ended this way — 4 for 4 across two pod
instances**, so treat a formed Alpha cluster as a few seconds of usable wire, not
a standing lab. Counting it on a node: `strings <console.log> | grep -c PROCGONE`
against `grep -c "Now a VMScluster member"`.

**This is an emulator defect, not a configuration error.** The cluster
negotiated, agreed membership, and completed a state transition first. Next
thing to try: both nodes MSCP-serve their disks (`%MSCPLOAD-I-CONFIGSCAN,
enabled automatic disk serving`) and #83 is a disk-I/O bug, so set `MSCP_LOAD=0`
via SYSGEN on both — each node has its own local system disk and cross-serving
buys this lab nothing.

### How the cluster was configured

`@SYS$MANAGER:CLUSTER_CONFIG_LAN.COM` on each node: form/join, LAN comms yes,
IP comms no, boot server yes, `ALLOCLASS 0`, no quorum disk, `EXPECTED_VOTES 2`,
group number **2026**, password **OVMXALPHACLU**. Then AUTOGEN + reboot.

Nodes cloned from the golden image all carry `ALPHA1`/2049, so **node 2 must be
re-stamped before it can coexist**:

```
$ RUN SYS$SYSTEM:SYSGEN
SYSGEN> USE CURRENT
SYSGEN> SET SCSNODE "ALPHA2"
SYSGEN> SET SCSSYSTEMID 2050
SYSGEN> WRITE CURRENT
```
then reboot. Convention: `ALPHA1/2/3` at DECnet 2.1/2.2/2.3 → 2049/2050/2051,
clear of lab-1's 1025–1329.

### The networking trap: veth, never tap

lab-2 gives SIMH a **tap** because SIMH opens the tap's *character device* and
becomes the endpoint. **AXPbox cannot use a tap.** It drives its DE500 through
libpcap, and pcap injection goes out the interface's *transmit* path — on a tap
that means "hand the frame to whoever holds the chardev", which is nobody. Every
guest frame is dropped into an unread fd.

The symptom is maximally misleading: OpenVMS reports `%EWA0, Link state: UP`,
`CLUSTER_CONFIG` succeeds, and the node sits at `waiting to form or join an
OpenVMS Cluster` — while `tcpdump` on both `br0` and `tap1` captures **zero
packets in either direction**. `br0` also stays `NO-CARRIER`, which is the tell.

A **veth pair** has a real peer: inject on `veth<i>` → received by `vbr<i>` →
bridge forwards normally. That one change took the lab from zero packets to a
formed cluster.

### Still open

**A cluster needs a licence.** These nodes run unlicensed
(`%LICENSE-E-NOAUTH`), which permits console login and was enough to configure
and form the cluster above — but VMScluster is a licensed facility and this is
not a supported configuration. Lab-1's `PAKS.COM` holds 99 **VAX** PAKs; Alpha
PAKs are a separate grant. The clean source is the VSI Community Licence
Programme, which issues Alpha PAKs free for non-commercial use. **Operator
decision — do not work around it.**

### Next step on the cluster

Set `MSCP_LOAD=0` via SYSGEN on both nodes and re-form. Both currently MSCP-serve
their disks (`%MSCPLOAD-I-CONFIGSCAN, enabled automatic disk serving`) and
AXPbox #83 is a disk-I/O bug, so the serving path is the first suspect for the
`PROCGONE` crash. Each node has its own local system disk; cross-serving buys
this lab nothing.

### Why no DECnet

AXPbox has two open issues (#39, #84) where starting DECnet Phase IV
machine-checks OpenVMS 8.4 (`MACHINECHK`, bugcheck 0x215), and one (#61) where
DECterm does the same. Installing DECnet would have put a known crash inside the
golden image for no benefit, since clustering uses SCS rather than DECnet. It can
be added later to a clone if something actually needs it.

## Files

| Path | What |
|---|---|
| `Dockerfile` | Node image: both AXPbox builds + `srmdrv.py` + entrypoint. |
| `entrypoint.sh` | One pod = one Alpha. Per-replica disk/NVRAM, generated config, persistent console, optional unattended VMS boot. |
| `k8s/20-alphalab.yaml` | Headless Service + StatefulSet. Reuses lab-2's namespace and PVC. |
| `tools/srmdrv.py` | Console driver over TCP. Strips telnet negotiation, tees to a log, gates commands on the prompt, `--fifo` mode for a long-lived console. |
| `setup.sh` | Bare-metal (non-k3s) setup, for running an Alpha directly on a host. |
| `node.sh` | Bare-metal lifecycle: `start`/`send`/`log`/`status`/`stop`. |
| `rip-media.sh` | Faithful raw `dd` rip of OpenVMS media on a machine with an optical drive. |
| `cfg/*.cfg` | Hand-written configs for the bare-metal path. The k3s path generates its own. |

`setup.sh` / `node.sh` / `cfg/` are the **bare-metal** path used to bring the
first Alpha up on workshop. They still work, but the dev seat is the wrong place
to run one — prefer the pod.
