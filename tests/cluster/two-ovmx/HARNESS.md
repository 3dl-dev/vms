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

## Baseline finding (rd vms-f3e, 2026-08-28)

Ran the harness against **unmodified main** SCSD.EXE (default flags). Result:

**JOIN DOES NOT COMPLETE.** Both nodes stall at **rung 0 — NISCA channel
formation.** Each node emits its multicast HELLO and *receives* the other's
(the bridge floods `0x6007` correctly: node A logs `SCSD-I-FRAME class=HELLO
… dst=ab:00:04:01:01:01` from node B's MAC), but neither ever exchanges a
**directed** HELLO. Nothing climbs past `HELLOSENT`; all DGRAM/CM counters
stay 0.

**Root cause — OVMX SCSD has no MEMBER/initiator role; it is a pure
RESPONDER.** The whole choreography was tuned against a real VAX, where the
running *member* drives every initiator step and OVMX (the joiner) only
responds. Two symmetric OVMX joiners have no member, so the handshake
deadlocks at the first step that needs an initiator — and again at every later
initiator step. Two concrete manifestations, same cause:

1. **Rung 0 — the member-side SOLICIT is missing.** The receive dispatch acts
   only on frames *unicast to our HW MAC* (`scsd.c` ~line 8115: "our own
   multicast beacon prompts the peer's directed HELLO"). A received *multicast*
   HELLO returns immediately. OVMX never turns a heard multicast HELLO into a
   soliciting directed HELLO (abs-30 = `PFW_INIT`/0xb2, the documented
   "member's first directed contact"). Both nodes wait forever for a directed
   HELLO neither will send.

2. **Rung ~VC — the first 0x41 START is never initiated.** Even with the
   channel up, `scsd.c` ~line 9425 sends OVMX's own START *only when a peer
   START arrives* ("OVMX issues its own START only when a peer identity-bearing
   frame arrives"). The member (VAX) normally sends START round-0 first; two
   OVMX nodes both wait. Since `ps->start_acked` gates the joiner
   CONNECT-REQUEST and the whole join sequencer, nothing downstream can fire.

### PoC that confirms the diagnosis: `OVMX_MCAST_SOLICIT`

`src/vmsscs/scsd.c` carries a **minimal, kill-switch-gated** member-side
solicit (rd vms-f3e): when `OVMX_MCAST_SOLICIT` is set, a heard multicast HELLO
from another node triggers one directed HELLO (`PFW_INIT`/0xb2 — grounded, not
invented) per peer per second until the channel is up. **Absent the flag the
wire toward a real VAX is byte-identical** (a VAX is the member and does the
soliciting; OVMX must not also solicit it).

```bash
SCSD_ENV="OVMX_MCAST_SOLICIT=1" tests/cluster/two-ovmx/run.sh 60
```

With the flag, both nodes climb rung 0 → **directed HELLO exchanged + padded
HELLO size-verify complete** (`DIRHELLO`, `PADINIT`, `PADACK`; pcap ~44→~793
`0x6007` frames). It then stalls at manifestation #2 (no START initiated →
`start_acked` never set → no CONNREQ) — the **next** rung-1b gap to close,
which needs the same member-role initiator for the 0x41 START (grounded on
`scs_start_build` config-round-0) and should ride the same kill-switch.

**Bottom line for rung-1b (DLM):** two OVMX SCS nodes cannot today complete the
join against each other; a member-role initiator (solicit + first-START, both
kill-switch-gated) must land before any two-node DLM round-trip is testable.
This harness is the oracle that will prove each rung as it lands.

## rung-VC landed (rd vms-d60, 2026-08-28)

The member-role **0x41 START INITIATE** closes gap #2. Once the channel is up,
OVMX now issues its OWN round-0 START off the main-loop tick
(`scsd_start_initiate_for_peer`) instead of only reflecting a peer's — plus a
companion fix: in the simultaneous-START collision both nodes reach VC OPEN via
the peer's round-1 **STACK** (FSM action `SEND_ACK`), not the round-2 **ACK**
the default latch waits for, so `scsd_vc_settle` now treats that `SEND_ACK` as
round-2-ack-due (member-role-gated). Both under the same `OVMX_MCAST_SOLICIT`
umbrella (`OVMX_NO_START_INITIATE` suppresses just the initiate).

```bash
SCSD_ENV="OVMX_MCAST_SOLICIT=1" tests/cluster/two-ovmx/run.sh 60                 # rung-VC
SCSD_ENV="OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1" tests/cluster/two-ovmx/run.sh 60 # + sequencer
```

Proven on the harness:

- **rung-VC complete:** both nodes reach `STARTTX (initiated)` → `STARTRX` →
  `VCFSM STACK→ACK` → **VC OPEN + `start_acked=1`** (`SCSD-I-STARTDONE`,
  `SCSD-I-VCOPEN`). `members_reached=1`.
- **the join sequencer then FIRES** (`OVMX_JOIN_SEQ=1`) and climbs **5 of 8
  steps**: SCS$DIRECTORY connect **accepted both ways** (`OWNDIRBOUND`),
  MSCP$TAPE miss → MSCP$DISK lookup HIT → **MSCP$DISK connect accepted**
  (`MSCPBOUND`), VMS$VAXcluster lookup HIT → **VC connect sent (step 7/8)**.
- **next stall = the next rung:** it retransmits step 6/7 to `JOIN_RETX_MAX` —
  the peer transport-accepts OVMX's VMS$VAXcluster VC connect but never sends
  `ACCEPT_REQUEST` for a connect it did not itself solicit (`joiner=CONNECT
  SENT, connected=no`). That is the **member-side VMS$VAXcluster accept /
  add-member** rung — the next member-role item, not rung-VC.
- **flag-off byte-identical:** re-run with the flag absent → only the multicast
  HELLO beacon; **zero** `STARTTX`/`DIRHELLO`/`MCASTSOLICIT`, no new frames. The
  merged OVMX↔VAX path is unperturbed (OVMX still only reflects a peer's START).

So OVMX↔OVMX now completes rung-0 + rung-VC and drives the joiner choreography
through MSCP$DISK; what remains before a two-node DLM round-trip is the
member-side SYSAP-accept rung.

## Kill-switch discipline

Any fix that this harness motivates for the OVMX↔OVMX path MUST follow the
codebase's `OVMX_*` env kill-switch discipline: when the flag is absent, OVMX's
bytes on the wire toward a real VAX stay identical. This harness changes no
production code — it only observes.
