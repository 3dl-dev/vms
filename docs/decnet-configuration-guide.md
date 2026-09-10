# OVMX DECnet Configuration Guide

This guide is for an operator configuring **DECnet Phase IV** on OVMX. It covers
exactly what ships: how `NCP` configures a node, how outbound and inbound
`SET HOST` work today, and what a DECnet-mediated file `COPY` does and does not
do. Just as important, it is explicit about what a real DECnet Phase IV network
does that OVMX does **not** do yet — see
[Current status & limitations](#current-status--limitations). Nothing below is
described as working unless a real code path ships it over the real executive
(Rule 9 / INV-6); where a capability is partial or not yet built, this guide says
so plainly rather than describing it as if it worked.

> **Scope.** This is the *operator* view — the DECnet sibling of
> [`docs/cluster-configuration-guide.md`](cluster-configuration-guide.md). The
> architecture ruling and clean-room provenance live in
> [`docs/design-decnet-ovmx.md`](design-decnet-ovmx.md) and
> [`docs/design/faithful-sessions-and-network-subsystems.md`](design/faithful-sessions-and-network-subsystems.md)
> (rd vms-515); the byte-level wire provenance lives in
> [`docs/decnet-provenance-register.md`](decnet-provenance-register.md). The
> single source of truth for what is real vs. partial vs. absent is the
> Compatibility Surface Register —
> [`docs/compat/facilities/decnet.yaml`](compat/facilities/decnet.yaml) /
> [`docs/compatibility-surface.md`](compatibility-surface.md#decnet--decnet-phase-iv-routing-nsp-task-to-task-set-host) —
> and every claim below is sourced from it. This guide is prose and worked
> examples; it does not duplicate the register as a second ledger.

## 1. Overview and architecture

DECnet Phase IV in OVMX is a layered product over the executive, not a bolt-on
socket library. Three pieces matter to an operator:

- **NETACP** is a privileged `RUN/DETACHED` process that owns the DECnet device
  face (`_NET:`, layered over the same NIC `ETH0:` rides) and the Session
  Control **object-dispatch table** — analogous to how `scsd` owns the cluster's
  SCS surface. NETACP's own control path never parses attacker-controlled wire
  bytes: a low-privilege engine decodes the wire and hands NETACP only a
  validated, typed connection descriptor (`decnet$wire-isolation`, implemented).
- **The wire engine** is a userspace Phase IV implementation — datalink, NSP
  logical links, and routing/HELLO adjacency — over `AF_PACKET SOCK_RAW`
  (ethertype `0x6003`), hidden entirely behind the executive device face. This
  is a deliberate substitution: Linux removed its in-kernel `AF_DECnet` stack in
  kernel 6.1, so unlike TCP/IP there is no live kernel facility left to ride
  (ruling vms-a1c; full rationale in `design-decnet-ovmx.md` §2). Nothing above
  NETACP ever sees a Linux socket.
- **The ONE authenticator.** Every DECnet path that grants access checks
  credentials against the same SYSUAF/Purdy authority — never a private
  credential path of its own. An interactive session (inbound `SET HOST`/CTERM)
  runs the real `SYS$SYSTEM:LOGINOUT.EXE` via `$CREPRC PRC$M_INTER|PRC$M_LOGINOUT`
  — the same primitive a console login uses — which prompts and authenticates
  fresh. Inbound FAL file access instead checks the connect-carried credentials
  directly against that same SYSUAF (`sysuaf_authenticate` + the disabled-account
  gate LOGINOUT and SSH also enforce) before serving a file. Same authority, one
  authenticator; the interactive paths reach it through LOGINOUT, FAL reaches it
  directly because a file transfer has no interactive login.

**Phase-IV addressing.** A node address is `area.node` (area 1–63, node
1–1023). Ethernet MACs are the algorithmic Phase IV form
`AA-00-04-00-<area.node, little-endian>` — e.g. node 1.2 is `aa:00:04:00:02:04`
— the same scheme the cluster's LAVC/SCS transport borrows for its own logical
MACs. This is oracle-observed against real OpenVMS VAX nodes, not an OVMX
invention (`docs/oracle/vax-sethost-cterm.md`, `docs/oracle/vax-copy-fal-dap.md`).

**Phase IV only.** DECnet-Plus (Phase V / OSI, `NCL`) is out of scope; Phase IV
is what the VAX/Alpha lab nodes speak and what NETACP's engine speaks
(`design-decnet-ovmx.md` §1). "DECnet" is a DEC/HPE/VSI trademark — OVMX badges
its implementation "DECnet-compatible networking," never claiming the mark
itself (INV-0).

## 2. Configuration with NCP

`NCP` is the real Network Control Program: a persisted node database plus
executor (local-node) configuration, driven one command per invocation
(`MCR NCP <command...>`), grammar from the public *DECnet for OpenVMS
Networking Manual*. Register row: `decnet$ncp`, **partial/real**.

### Executor identity

```
$ MCR NCP SET EXECUTOR ADDRESS 1.2
$ MCR NCP SET EXECUTOR NAME VAX2
$ MCR NCP SET EXECUTOR STATE ON
$ MCR NCP SHOW EXECUTOR CHARACTERISTICS

Node Volatile Characteristics

Executor node = 1.2 (VAX2)
State                    = on
Identification           = OVMX DECnet-compatible networking
```

### The remote-node database

`SET`/`DEFINE NODE` add or update an entry; `SHOW KNOWN NODES` / `SHOW NODE`
read it back; `CLEAR`/`PURGE NODE` remove one. This database is what
`SET HOST <node>` and a `NODE::` filespec resolve a name or address through
(`decnet$node-database`, implemented).

```
$ MCR NCP SET NODE 1.2 NAME VAX2
$ MCR NCP SHOW KNOWN NODES

Known Node Volatile Summary

Node         Name

1.2          VAX2

$ MCR NCP SHOW NODE VAX2
1.2          VAX2

$ MCR NCP CLEAR NODE VAX2
```
A node may be looked up and cleared by either its `area.node` address or its
name — both forms accept either.

### What NCP does not yet do

- **No SET/DEFINE split.** Real NCP's `SET` (volatile, until reboot) and
  `DEFINE` (permanent, in the permanent database) act on two databases; OVMX
  keeps a single persisted database, so `SET` and `DEFINE` both write the same
  store today.
- **No circuits, objects, lines, counters, or LOOP.** NCP configures the node
  database and the executor's address/name/state only. There is no
  `SET CIRCUIT`, `SET OBJECT`, `SET LINE`, `SHOW ... COUNTERS`, or `LOOP`
  surface.
- **NCP is config-only.** It does not itself bring the network up or down —
  it edits the database NETACP and the engine read.
- **Storage-location caveat (rd vms-20e).** The node database and executor
  configuration persist at a Linux host path (`/etc/ovmx/decnet/` by default,
  overridable with `OVMX_DECNET_NODEDB` / `OVMX_DECNET_EXECUTOR`), not through
  the VMS file layer at `SYS$SYSTEM:NETNODE_REMOTE.DAT`. The data and the
  command surface are real; the on-disk location is an OVMX-choice
  faithfulness gap tracked separately.

## 3. Remote interactive login — SET HOST

### Outbound: `$ SET HOST <node>`

```
$ SET HOST VAX2
```

DCL activates `SYS$SYSTEM:DECNETD.EXE --set-host <node>` on the caller's
terminal through the real executive image activator (`dcl_activate_image` →
`imgact_activate` — the same path `RUN` uses), which opens a genuine NSP
logical link to the remote's Session Control **object 42 (CTERM)** and bridges
the local terminal to it using VMS-native `$QIO` terminal I/O (`IO$_SETMODE`
pass-all + `IO$_READVBLK`/`IO$_WRITEVBLK` through the executive terminal
driver) — never raw `tcsetattr`/`cfmakeraw` above the VMS layer. An optional
`/USERNAME=name` qualifier sets the connect-carried source identity, which is
**proxy/accounting information only** — the remote always authenticates fresh
(see below). On teardown, control returns with the canonical
`%REM-S-END, control returned to node <local>::`
(`docs/oracle/vax-sethost-cterm.console.txt`). If `DECNETD.EXE` is not staged
on the system disk, `SET HOST` reports
`%SET-I-NOTAVAIL, DECnet is not available on this system` rather than faking a
session. Register row: `decnet$set-host`, **partial/real**.

### Inbound: an authenticated LOGINOUT, not an auto-login

A remote node's object-42 connect is accepted by NETACP and mints a real
`RTAn:` virtual terminal *through the executive*
(`VMS_IOCTL_TERM_CREATE` → `vms_devtab_add_terminal`, cross-process visible via
`$GETDVI` from a different process than the session), then runs the real
`SYS$SYSTEM:LOGINOUT.EXE` on it via `$CREPRC PRC$M_INTER|PRC$M_LOGINOUT` — the
same primitive the console login path uses. This matches real OpenVMS
behavior exactly: the connect carries the source `node::user` identity (shown
later as `Remote Port Info` on `SHOW TERMINAL`), but that identity is **never**
consumed as a credential — `LOGINOUT` always prompts a fresh `Username:` and
`Password:`, and a bad password is refused
(`docs/oracle/vax-sethost-cterm.md` §1). Register row:
`decnet$cterm-session-auth`, **implemented/real**.

### Honest state

OVMX-to-OVMX `SET HOST` (client driving a genuine Connect Initiate to object
42 and the server refusing honestly without an executive) is proven in
`tests/integration/decnet_set_host_live.sh`. **Not yet proven:** the full
authenticated round trip (client → real remote `LOGINOUT` challenge →
`%REM-S-END`) against a *live VAX* in the loop, and a CI leg carrying both
`CAP_NET` and a real `/dev/vms` executive with `DECNETD.EXE` staged into the
boot image — both tracked as follow-ons on the `decnet$set-host` register row.
Only one CTERM session at a time is supported, and there is no
read-solicitation byte-transparent pump yet on the inbound side.

## 4. File access — COPY over DECnet (FAL/DAP)

The **inbound** side works: OVMX runs an authenticated FAL (File Access
Listener) server on DECnet Session Control **object 17**, and a real DAP (Data
Access Protocol) codec moves the file through RMS over the ODS-2 ACP
(`decnet$fal`, **partial/real**; `decnet$dap`, **implemented/real**). The
**outbound** half — wiring DCL's own `COPY` command to drive a client over the
live datalink — is not built yet (see "What does not work yet" below).

### Inbound FAL — authenticated at the connect, the opposite of CTERM

The FAL connect is the mirror image of SET HOST/CTERM (§3). Where CTERM carried
**empty** access-control fields and the remote LOGINOUT prompted fresh, a FAL
`COPY` connect (object 17) carries the username **and password** in the NSP
connect-initiate's access-control fields, and **FAL authenticates from those
connect-time credentials — it does not prompt** (the client syntax that puts
them there is `COPY file.txt node"user password"::dest.txt`). This is the fact
the oracle `docs/oracle/vax-copy-fal-dap.md` (rd vms-cd3) established, and the
FAL server reproduces it faithfully:

- FAL decodes the connect-carried username+password with a **bounded** decoder
  (attacker-controlled bytes: fuzzed 200k inputs + every truncated prefix,
  ASan/UBSan-clean) and authenticates them through **the one faithful
  authenticator** — `sysuaf_lookup` + `sysuaf_authenticate` (Purdy) + the
  disabled-account gate, the *same* SYSUAF path LOGINOUT and SSH use — **before
  accepting the logical link**. A bad password, an unknown user, or a
  `DISUSER`/`DISACNT` account is **refused with an NSP disconnect — no
  connect-confirm, no DAP, no file** (INV-6: no file is served on an
  unauthenticated connect — the FAL analogue of the SET HOST no-auth hole,
  closed the same way).
- Once authenticated, FAL speaks DAP over the NSP logical link — file-attribute
  negotiation (name, resolved full spec, owner UIC, RMS attributes), then
  verbatim record data, then a status/access-complete exchange, then teardown —
  the public DAP message set, decoded **clean-room** against the oracle's
  captured bytes (Rule 8; the password value in the capture is redacted per
  INV-0 — only the wire structure + the carried-cleartext semantic are kept).
- The transferred bytes land in a real file via RMS over the ODS-2 ACP — no
  userspace fake; where no executive is present the path fails honestly (Rule 9).

### What does not work yet

- **DCL `COPY NODE::` (outbound).** DCL recognizes a node-prefixed filespec but
  does **not** yet drive an outbound FAL client over the datalink: a
  `COPY VAX2"user pw"::file.txt local.txt` reports `%COPY-I-NETNOTWIRED` rather
  than transferring — an honest "not wired," not a broken transfer. The outbound
  DCL→datalink COPY bridge is a tracked follow-on (**rd vms-ea8**).
- **Advanced DAP** — indexed/relative files, wildcards, `DIRECTORY`, block mode,
  and proxy access (an empty-access-control connect matched against a proxy DB,
  instead of a cleartext password) — is not built; the current rung handles the
  sequential-file case the oracle captured.
- **Byte-level stock-VAX FAL interop.** Two OVMX nodes interoperate over DAP
  faithfully; the per-field DAP sub-framing is not yet asserted byte-identical
  to a stock OpenVMS VAX FAL (a filed follow-on).

The end-to-end proof (a sequential file transferred both directions with real
SYSUAF/Purdy auth — GUEST accepted, wrong password and `DISUSER` refused — and
byte-verified through real RMS) runs on a real executive in CI
(`DECNETD.EXE --fal-accept-test` in the booted acceptance battery); the
no-executive floor (`--fal-selftest`: a real object-17 connect carrying creds,
the honest refusal, the DAP-over-NSP pump) runs anywhere.

## 5. Current status & limitations

Sourced from [`docs/compat/facilities/decnet.yaml`](compat/facilities/decnet.yaml)
(full detail and evidence pointers there — this table is a summary, not a
second ledger).

| Capability | Status | Authenticity | What this means for you |
|---|---|---|---|
| Ethernet Endnode Hello codec | verified | real | Oracle-verified byte-identical; library only, no live adjacency use documented here. |
| Router Hello codec | implemented | real | Spec-derived, self-round-trip tested; not oracle-anchored. |
| Routing adjacency state machine | implemented | real | DOWN/INITIALIZING/UP with hello/listen timers. |
| NSP transport codec | partial | real | Connect Initiate oracle-verified; other PDUs self-round-trip only. Codec, not yet a general-purpose live transport engine outside SET HOST. |
| Inbound FAL file server (object 17) | partial | real | §4. Authenticated at the connect (SYSUAF/Purdy + disabled gate, bad password refused before accept); serves/stores via RMS over the ACP. Outbound DCL `COPY` bridge is a follow-on (rd vms-ea8). |
| DAP codec | implemented | real | §4. Bounded, fuzz-clean; oracle-verified message sequence + carried values, public-spec field framing. |
| Task-to-task programmatic `$QIO` | **absent** | n/a | Generic user-program logical-link `$QIO` to a DECnet object is not built (distinct from FAL, which is real above). |
| NCP (node/executor config) | partial | real | §2 above. No circuits/objects/lines/counters/LOOP; single persisted DB; node DB stored at a Linux path, not `NETNODE_REMOTE.DAT` (rd vms-20e). |
| Node database + name↔address resolution | implemented | real | Backs NCP and `SET HOST`/`NODE::` resolution. |
| Session Control CONNECT codec | verified | real | Oracle byte-identical against a real VAX capture; access-control fields correctly empty for CTERM. |
| Inbound SET HOST (CTERM session auth) | implemented | real | §3 above. Fresh LOGINOUT auth; one session at a time; no live-VAX bracket proof yet. |
| NETACP device face (`_NET:` + object dispatch) | implemented | real | Executive-resident, cross-process real; object registry query and the `NETACP.EXE` rename are follow-ons. |
| Wire-parsing isolation (A2/A8) | implemented | real | Attacker bytes never reach NETACP's privileged path unvalidated. |
| Outbound SET HOST (client) | partial | real | §3 above. OVMX↔OVMX proven; live-VAX + full-executive CI leg is a tracked follow-on. |
| `NODE"acc"::` filespec syntax | partial | real | Parses and reconstructs; DCL `COPY` acts on it but reports `%COPY-I-NETNOTWIRED` — the outbound transfer bridge is a follow-on (rd vms-ea8). |

**Bottom line for an operator today:** you can configure node identity and a
node database with `NCP`; log in to a remote OVMX node with outbound or inbound
`SET HOST` (each side authenticating through the real LOGINOUT/SYSUAF path); and
**receive** a file from a remote node into OVMX over an authenticated inbound
FAL/DAP transfer. You cannot yet **initiate** a file transfer with DCL `COPY`
over `NODE::` (it reports `%COPY-I-NETNOTWIRED`; the outbound bridge is rd
vms-ea8), run task-to-task `$QIO` applications, or manage
circuits/objects/lines through `NCP`.

## Clean-room provenance (Rule 8)

Everything OVMX knows about the DECnet Phase IV wire — datalink framing, NSP,
routing HELLO, the Session Control CONNECT layout, and now the FAL/DAP
credential and message sequence — is derived only from (a) observing the wire
against real OpenVMS VAX nodes on the reference lab and (b) public DNA Phase
IV / DAP specifications. No VSI/HPE VMS source or binary was disassembled,
decompiled, or consulted. Full provenance and field-by-field grounding:
[`docs/decnet-provenance-register.md`](decnet-provenance-register.md); the two
worked oracle captures behind this guide's SET HOST and COPY sections are
[`docs/oracle/vax-sethost-cterm.md`](oracle/vax-sethost-cterm.md) and
[`docs/oracle/vax-copy-fal-dap.md`](oracle/vax-copy-fal-dap.md).
