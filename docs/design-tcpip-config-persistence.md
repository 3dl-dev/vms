# TCP/IP config persistence across reboot (persist over the ACP + boot reapply)

**rd:** epic `vms-a0b2` (parent `vms-67f`, the TCP/IP layered product) — pieces
`vms-210` (P1), `vms-b679` (P2), `vms-b97` (P3).
**Status:** design (P0). Implementation gated on `vms-402` (HOST.DAT ACP flip)
reaping — the boot reader (P2) reuses the same `rms_textfile`-over-ACP read
pattern that PR proves.

## The gap

`TCPIP SET ROUTE`, `SET INTERFACE`, and `SET NAME_SERVICE` apply their effect to
the **live** substrate — `ip route replace`, `SIOCSIFADDR`/`IFF_UP`,
`/etc/resolv.conf`, and (for the interface address) the executive-resident
`TCPIP$INET_HOSTADDR` logical. None of that survives a reboot:

- The live effects are runtime state, lost on the next boot.
- Each verb also writes a `.DAT` file — `TCPIP$ROUTE.DAT`,
  `TCPIP$INTERFACE.DAT`, `TCPIP$NAMESERVICE.DAT` — intended as the persistent
  record. But those writes go through `fopen(VMS_SYSTEM_DIR/…)` and
  `VMS_SYSTEM_DIR = SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE"` with
  `SYSDISK_MOUNT = "/vms"` — the **retired POSIX passthrough** (vms-37e). On the
  booted runtime `/vms` does not exist, so the write silently does nothing.
- Worse, **nothing reads those `.DAT` files back** anyway: `SHOW ROUTE` reads
  `/proc/net/route`, `SHOW CONFIGURATION` reads the executive `TCPIP$` logicals,
  `SHOW INTERFACE` reads `SIOCGIF*`, and no boot step reapplies them. They are
  **write-only dead stores.**

On real OpenVMS, `TCPIP$CONFIG` persists the configuration and `TCPIP$STARTUP`
reapplies it at every boot. OVMX must do the same for the config to be faithful
and for a configured node to come back configured.

(`TCPIP$HOST.DAT` was the exception — `SHOW HOST` reads it — which is why its
ACP flip, `vms-402`, was a clean read+write round-trip. These three have no read
side, so persistence only becomes meaningful once a **reader/reapply** exists.)

## Design

Two halves, mirroring real VMS: **persist** (write the config the VMS way, over
the ACP) and **reapply** (read it back at boot and re-run the SET verbs).

### 1. Persist the writes over the ACP (P1, `vms-210`)

Flip the three `.DAT` writers in `src/vmsdcl/dcl_cmd_misc.c` from `fopen` to
`rms_textfile` (RMS over the Files-11 ACP, the `vms-402` / `vms-0a5` / `vms-4ac`
pattern):

| Verb | Store | Semantics | Call |
|------|-------|-----------|------|
| `SET ROUTE` | `SYS$SYSTEM:TCPIP$ROUTE.DAT` | append (one route per line) | `rms_textfile_append_line` |
| `SET INTERFACE` | `SYS$SYSTEM:TCPIP$INTERFACE.DAT` | append (one iface per line) | `rms_textfile_append_line` |
| `SET NAME_SERVICE` | `SYS$SYSTEM:TCPIP$NAMESERVICE.DAT` | supersede (single record) | `rms_textfile_write_line` |

- `SYS$SYSTEM:` resolves through `LNM$FILE_DEV` (honours `DEFINE/SYSTEM
  SYS$SYSTEM`), so the config-dir-as-logical faithfulness intent is satisfied.
- **Fail-honest** on the `__linux__` ACP build: no POSIX fallback, so with no
  executive/ACP volume the write returns `-1`. The live apply is unchanged; only
  the persisted-record write moves to the ACP. Report the persistence outcome
  honestly (do not claim a durable record was written when it was not) — the
  live-apply status is separate and already reported by each verb.
- Record formats (unchanged, so a future reader parses what SET wrote):
  - route: `DEFAULT <gw>` or `<dest> <gw>[ <mask>]`
  - interface: `<vms-ifname> <addr>[ <mask>]`
  - nameservice: `SERVER=<ip>` then optional `DOMAIN=<domain>`

**Test:** a `/dev/vms` ctest (clone of `tests/qemu/test_syssvc_tcpip_host_acp.c`)
that appends/writes each store on the mounted `VDA0:` volume, reads it back
byte-exact, and fails-honest when dismounted. Runs in the KE shard leg.

### 2. Reapply at boot (P2, `vms-b679`)

Add a reapply step the VMS-faithful way: `SYS$STARTUP:TCPIP$STARTUP.COM` reads
the persisted stores over the ACP (DCL `OPEN`/`READ`, the `dcl_rms_read` seam)
and re-runs the equivalent SET verb for each record:

```
$ OPEN/READ/ERROR=done RF SYS$SYSTEM:TCPIP$ROUTE.DAT
$loop: READ/END=done RF line
$      ! parse "DEFAULT gw" | "dest gw [mask]" -> TCPIP SET ROUTE ...
$      GOTO loop
$done: CLOSE RF
```

…and likewise for `TCPIP$INTERFACE.DAT` (→ `TCPIP SET INTERFACE`) and
`TCPIP$NAMESERVICE.DAT` (→ `TCPIP SET NAME_SERVICE`). Absent file ⇒ honest no-op
(nothing to reapply), never an error. This is a distinct step from **service
enablement** (the shipped-disabled posture, `test_tcpip_posture_guard.sh`) —
reapplying the operator's saved *configuration* is not the same as auto-starting
a *service*, and it stays within that posture.

**Test:** a DCL test that the reapply procedure, given a fixture store, issues
the correct `TCPIP SET …` commands (and no-ops honestly when the store is
absent). Host-testable against the DCL harness with a fixture; the full
cross-reboot proof is P3.

### 3. Full-boot e2e (P3, `vms-b97`) — LAST

`OVMX_QEMU_FULL_E2E` leg (models `test_dcl_acceptance_e2e` /
`run_tcpip_daytime_boot_e2e`): boot the mastered image, `TCPIP SET ROUTE` +
`SET INTERFACE` + `SET NAME_SERVICE`, **reboot**, assert `SHOW ROUTE` /
`SHOW INTERFACE` / `SHOW CONFIGURATION` reflect the reapplied config. Heavy /
OOM-prone → CI full-e2e leg only, done last.

## Pivotability (SSH / vms-16b)

The pieces are ordered so an SSH pivot pauses cleanly:
P0 (this doc) → P1 (writes→ACP, bounded ctest) → P2 (boot reapply, bounded DCL
test) → **P3 (heavy full-boot e2e) last**. If Baron rules `vms-16b` mid-way,
pause at the last completed piece (each is independently green-by-SHA) and pivot
to the SSH→DCL handoff (the headline); config work resumes at the next piece.
