# DECnet task-to-task — the `_NET:` $QIO seam + inbound object activation

**Status:** design (rung-1 remainder of the DECnet ladder, epic vms-30e). Drafted by
the DECnet lane 2026-09-14 at the conductor's request; to be decomposed into rd
children of vms-30e. Clean-room (Rule 8); executive-resident driver (INV-6, Rule 9).

## Why this is a design, not a codec-bind

The conductor's menu framed task-to-task as "codecs exist, no engine binds them."
Grounding on origin/main shows that framing is **already satisfied for the pieces it
named**:

- The Session Control CONNECT descriptor codec is **generic**: `struct
  dnet_cterm_sc_connect` carries `dst_format` ∈ {`OBJECT`(0), `NAMED`(1), `CODED`(2)},
  `dst_object`, `dst_task`, and the source descriptor
  (`src/vmsdecnet/cterm/dnet_cterm.c`). Object 0 = TASK is declared
  (`DNET_OBJ_TASK`, `dnet_objectdb`).
- The NSP **logical-link FSM is real and drives a live link over the datalink**:
  `dnet_engine_link_open/accept/send/close/service/rx/tick`
  (`src/vmsdecnet/engine/dnet_engine.c`). CTERM (obj 42), FAL (obj 17), and the
  outbound COPY client (vms-ea8) all ride it today.
- The DECnet device face `_NET:` is executive-resident and `$ASSIGN`/`$GETDVI`-able
  cross-process (`src/kernel-core/vms_devtab.c`, `decnet$netacp-device-face`).

So the codecs and the link engine are **not** the gap. The genuine remainder of
task-to-task is two **executive seams** that no code crosses yet:

1. **Outbound/inbound `$QIO` on `_NET:`** — a *user process* (not the NETACP daemon)
   opening a logical link and doing byte-transparent read/write over it.
2. **Inbound generic-object activation** — an arriving CONNECT for a *declared* object
   other than 42/17 causing NETACP to *activate that object's servicing image* (the
   way object 42 → `$CREPRC` LOGINOUT on an `RTAn:`), rather than the hard-coded
   42/17 dispatch that exists today.

Both are architecture (device-driver `$QIO` function codes; a process-creation path
keyed off the object registry), which is why they must be designed and decomposed,
not big-banged.

## Target VMS behaviour (the thing we are reproducing)

Task-to-task on real VMS, two shapes:

- **Outbound (active) task:** `$ASSIGN` a channel to `NODE::"TASK=SERVER"` (or
  `NODE::"0=SERVER"`, or a numeric object), then `$QIO IO$_WRITEVBLK` /
  `IO$_READVBLK` to exchange messages; `$QIO IO$_ACCESS` carries the connect,
  `IO$_DEACCESS` disconnects. The channel is a `_NET:`/`NETn:` device.
- **Inbound (passive) task:** a program `$ASSIGN`s `SYS$NET` (or declares a network
  object via NCP `SET OBJECT ... FILE=`), `$QIO IO$_ACCESS` to *accept* the pending
  inbound connect, then reads/writes. NETACP, on an inbound CONNECT to a declared
  object, either hands it to a waiting declarer or **activates the object's image**
  (`$CREPRC` running `SYS$SYSTEM:<object-file>`) with `SYS$NET` bound to the link.

Two OVMX honesty rules bind every rung:
- **INV-6:** every wire field read from executive/link state; absence → honest
  `SS$_` failure, never a fabricated session. No file/task served on an
  unauthenticated inbound connect (the FAL/CTERM discipline).
- **Rule 9 / no userspace fallback:** the link and the device live in the executive;
  a `$QIO` with no `/dev/vms` fails honest.

## Proposed seam

### A. `_NET:` device `$QIO` function codes (the user-process face)

Add a DECnet logical-link `$QIO` path on the `_NET:`/`NETn:` device, dispatched by
the executive I/O database to the NETACP engine that owns the datalink:

| function | meaning | maps to |
|---|---|---|
| `IO$_ACCESS` (outbound) | open a logical link; P-args carry the target descriptor (`NODE::"TASK=x"` parsed by the generic SC codec) + optional access control | `dnet_engine_link_open` + pump to RUN |
| `IO$_ACCESS` (inbound) | accept the pending inbound connect on this channel | `dnet_engine_link_accept` |
| `IO$_WRITEVBLK` | send one message as an NSP data segment | `dnet_engine_link_send` |
| `IO$_READVBLK` | deliver the next inbound data segment | `dnet_engine_link_rx` → `rx_data` |
| `IO$_DEACCESS` | disconnect | `dnet_engine_link_close` |

**Key architectural question (needs a ruling):** where does the per-link state live so
it is cross-process real? Two options, mirroring the CTERM/FAL precedent:

- **Option 1 — NETACP-brokered (recommended, matches today's engine).** The link FSM
  stays in the NETACP engine process; `_NET:` `$QIO` from a user process is brokered
  to NETACP through the executive (a mailbox/AST or a `vms_kif` call), the way object
  42 mints an `RTAn:` and `$CREPRC`s LOGINOUT. Reuses the *entire* proven engine
  unchanged; the seam is the broker, not a new link implementation. The `$QIO`
  channel is the user's handle; NETACP does the framing.
- **Option 2 — link FSM in the executive.** Move `dnet_link`/`dnet_engine` link state
  into `vms.ko` so `_NET:` `$QIO` drives it directly (like the cluster DLM is
  executive-resident). Faithful but a large lift and duplicates a proven userspace
  engine; only justified if brokering proves too lossy for byte-transparency.

Recommendation: **Option 1**, decomposed as: (a1) a host/kmod contract for the
`_NET:` `$QIO` function codes + the broker primitive; (a2) the outbound
`IO$_ACCESS`→`WRITE`/`READ`→`DEACCESS` path with a host self-test (two engines, a
user-side `$QIO` shim, a message round-trip over the NSP link — the analogue of
`--copy-transport-selftest`); (a3) the inbound accept path.

### B. Inbound generic-object activation (the passive-task face)

Today `decnetd.c` dispatches object 42 (CTERM) and 17 (FAL) with hard-coded handlers.
Generalise: on an inbound CONNECT, resolve the destination descriptor against the NCP
**object registry** (`dnet_objectdb`, already persisted with number/name/FILE):

- object **declared with a FILE** → `$CREPRC` that image with `SYS$NET` bound to the
  accepted link (the object-42 `$CREPRC` primitive, generalised; the carried creds
  authenticated first where the object requires it — FAL's discipline).
- object **declared, no servicing model yet** → honest `SS$_` reject with the correct
  DNA disconnect reason (`OBJECT` unknown/unavailable), never a fabricated accept.
- object **not declared** → reject (the registry is the authority; INV-6).

Decompose as: (b1) registry-driven inbound dispatch replacing the hard-coded
42/17 branch (keeping 42/17 as registered handlers); (b2) the `$CREPRC`-with-`SYS$NET`
activation for a FILE-backed object; (b3) an object-registry ENUMERATION `$QIO`
(already filed as vms-d8d2) so a declarer/`SHOW` can read the live table.

## Test ladder (ground-source, host-first)

1. **Host:** a `--task-selftest` — two engines over a socketpair datalink; the active
   side opens a task-to-task link (generic object/`TASK=` descriptor), sends a
   message, the passive side accepts + echoes, byte-verified. Proves the generic
   descriptor + link path for a non-42/17 object with no executive (the
   `--copy-transport-selftest` pattern).
2. **Booted battery:** a real user-process `$ASSIGN _NET:"TASK=x"` + `$QIO` round-trip
   through the executive broker (Option 1), cross-process real (the `RTAn:`/`_NET:`
   `$GETDVI` tell).
3. **Lab (vms-101-gated):** interop with a real VAX task-to-task pair.

## Non-goals / adjacent (do not fold in)

- FAL/COPY (vms-ea8, this lane's current PR) — separate object (17), separate rung.
- SET HOST inbound bracket (vms-a70 dir B) — object 42, live-VAX gated.
- The object-registry enumeration `$QIO` is already vms-d8d2; (b3) is that item.
- DECNETD.EXE→NETACP.EXE rename (vms-e1c) is cosmetic, independent.

## Open rulings for the conductor / Baron

1. **Option 1 vs 2** for where link state lives (recommend 1).
2. Whether inbound FILE-backed object activation ships in the 1.0 track or is
   post-1.0 (it introduces a general `$CREPRC`-from-wire path; CTERM/FAL are the
   audited instances today).
