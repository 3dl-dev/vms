# Two-OVMX-node SCS harness (rd vms-f3e)

The **first** test harness that puts two OVMX SCSD daemons on one L2 segment and
lets them run the SCS join choreography **against each other**. Every existing
cluster harness (`tests/lab/`, lab-1, lab-Alpha) pairs OVMX with a *real* VAX:
the stop-and-wait join sequencer in `src/vmsscs/scsd.c` (`enum join_step`,
`scsd_join_retx_for_peer()`) was only ever tuned against OpenVMS VAX peers. This
harness front-loads the pivotal rung-1b (DLM) risk: **can two OVMX SCSD nodes
complete the existing join against one another at all?**

It is an **observation oracle** (CLAUDE.md INV-6 / Rule 8): real SCS frames on a
real bridge, captured to pcap. Nothing about membership is faked; the verdict is
read from the daemons' own logs and the wire.

## What it is (and is not)

- **Is:** two `SCSD.EXE --connect` userspace processes, distinct identities,
  on one bridge, with a pcap. Hermetic.
- **Is not:** a VAX lab. No SIMH, no QEMU, no disk images, no VAX. This is
  BUILD/TEST tooling (Rule 9): it builds + observes the real SCSD daemon; it is
  not an OVMX runtime.

## Topology

```
        br0 (bridge, multicast_snooping off)
        /                              \
     v0b                              v1b        (bridge ports)
      |                                |
     v0a  <-- SCSD-A binds AF_PACKET   v1a  <-- SCSD-B binds AF_PACKET
   OVMXA, SCSSYSTEMID 1601           OVMXB, SCSSYSTEMID 1602
   distinct MAC + Con.ID base        distinct MAC + Con.ID base
```

Both endpoints share the one L2 segment, so the LAVC/SCA multicast HELLO
(ethertype `0x6007`, dest `AB-00-04-01-01-01`) and every directed frame reach
each other. SCSD derives its MAC from the bound interface and its identity
(SCSNODE/SCSSYSTEMID → Con.ID base, logical LAVC addr) from `OVMX_SYSGEN_PATH`.

**Why veth, not taps** (`tests/lab/entrypoint.sh` uses taps): SIMH holds a
tap's fd, which raises the tap's carrier. SCSD binds AF_PACKET directly and
opens no tap fd, so a bare `ip tuntap` + `ip link set up` tap stays NO-CARRIER
and the qdisc drops its egress. A veth pair carries the instant **both** ends
are admin-up — no fd needed — so it is the correct model for two userspace
AF_PACKET peers.

## Requirements

- **`docker`** (workshop has docker, not podman).
- **`CAP_NET_ADMIN`** (create bridge/veth) + **`CAP_NET_RAW`** (AF_PACKET/
  SOCK_RAW). The container is **not** `--privileged` and needs **no**
  `/dev/net/tun`; it drops all other caps.
- **GitHub CI cannot run this today** — its runners do not grant NET_ADMIN/
  NET_RAW. Run it on the **workshop host** (`docker run --cap-add …`), or a
  **k3s pod** whose securityContext adds those two caps. Same reason
  `tests/lab/` runs only in-cluster.

## Run it

```bash
# baseline: UNMODIFIED main SCSD.EXE, default flags, 90 s
tests/cluster/two-ovmx/run.sh

# longer run
tests/cluster/two-ovmx/run.sh 180

# diagnosis run with the full join sequencer armed on both nodes
SCSD_ENV="OVMX_JOIN_SEQ=1" tests/cluster/two-ovmx/run.sh 180
```

Artifacts land in `tests/cluster/two-ovmx/out/`:

| file                    | what |
|-------------------------|------|
| `two-ovmx.pcap`         | every `0x6007` frame on br0, both directions |
| `scsd-OVMXA.log`        | node A's full run log (join_step transitions) |
| `scsd-OVMXB.log`        | node B's full run log |
| `sysgen-OVMX{A,B}.dat`  | the two identity stores that were used |
| `VERDICT.txt`           | the baseline verdict + per-node highest rung |

## Reading the verdict

`verdict.sh` climbs each node up the NEW→MEMBER ladder from its log markers:

```
HELLOSENT → DIRHELLO → CONNREQ → CONNRESP → VCOPEN → VAXCLMEMBER
          → STARTTX → STARTDONE → CMCONFIG → CLUSTATE
```

The **success oracle** is `SCSD-I-VAXCLMEMBER` on **both** nodes (each node's
`VMS$VAXcluster` SYSAP connection reached OPEN → each sees the other as a
member). If both reach it, the harness is ready to carry rung-1b's DLM
round-trip. If not, the verdict names the highest rung each node reached — the
join_step where the OVMX↔OVMX exchange stalls — and dumps the log tails so the
unanswered frame is visible.

## Identity stores

`mk_sysgen_scratch.py` writes a `SYSG` v2 `OVMXVMSSYS.PAR` store **from
scratch** (layout: `src/libvms/include/sysgen_params.h`) carrying only the four
params SCSD reads — `SCSNODE`, `SCSSYSTEMID`, `ALLOCLASS`, `RECNXINTERVAL`. It
deliberately does **not** depend on `tests/lab/tools/mk_sysgen.py`, which
patches a lab-1-only proven-good template, so this harness runs anywhere.
Identities must stay cluster-unique (distinct SCSNODE **and** SCSSYSTEMID) or a
real peer would refuse them with `%PEA0, Remote System Conflicts with Known
System`; the defaults (OVMXA/1601, OVMXB/1602) are distinct.

## Kill-switch discipline

Any fix that this harness motivates for the OVMX↔OVMX path MUST follow the
codebase's `OVMX_*` env kill-switch discipline: when the flag is absent, OVMX's
bytes on the wire toward a real VAX stay identical. This harness changes no
production code — it only observes.
