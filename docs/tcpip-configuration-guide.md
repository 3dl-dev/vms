# TCP/IP Configuration Guide

This is the operator guide for configuring TCP/IP Services on OVMX — the
`TCPIP$CONFIG`-equivalent flow, the `TCPIP$*` logicals, the `TCPIP` management
verb, `PING`, and the auxiliary server, **as they actually work at V0.6.**

TCP/IP Services is a layered product (rd epic `vms-67f`), built like real
OpenVMS: a configuration/management plane plus a set of services. This guide
documents only the shipped 0.6 surface. Everything designed but not yet shipped
is called out under [Not yet supported at 0.6](#not-yet-supported-at-06); for
the full design, see [`docs/design-tcpip-services-ovmx.md`](design-tcpip-services-ovmx.md).

> **Authenticity note (INV-6).** The configuration plane is executive-resident:
> host, domain, and address are stored as SYSTEM logicals in `LNM$SYSTEM` over
> the real executive at `/dev/vms`. With no executive present these commands
> fail honestly (`%TCPIP-W-NOEXEC` / `SS$_NOSUCHDEV`) — they never fabricate a
> configuration. Every command below is exercised in CI against a real
> `/dev/vms` (`tests/qemu/test_syssvc_tcpip_*`) or as a DCL host test
> (`tests/dcl/test_tcpip_*`).

---

## What ships at 0.6

| Capability | State | Notes |
|------------|-------|-------|
| `@SYS$MANAGER:TCPIP$CONFIG` core-environment wizard | Shipped | Sets host/domain/address; Client and Server menu options are no-ops. |
| `TCPIP$INET_HOST` / `TCPIP$INET_DOMAIN` / `TCPIP$INET_HOSTADDR` logicals | Shipped | Executive SYSTEM logicals in `LNM$SYSTEM` — the durable config store. |
| `TCPIP SET INTERFACE` / `SET ROUTE` / `SET HOST` / `SET NAME_SERVICE` | Shipped | Applies to the Linux substrate; requires privilege for `SET INTERFACE`. |
| `TCPIP SHOW INTERFACE` / `SHOW ROUTE` / `SHOW HOST` / `SHOW CONFIGURATION` / `SHOW VERSION` | Shipped | Read-only introspection with VMS device names. |
| `PING` | Shipped | Real ICMP echo over `BGn:`; IPv4 literal only (no name resolution). |
| `@SYS$STARTUP:TCPIP$STARTUP` + `TCPIP$INETD.EXE` (auxiliary server) | Shipped | DAYTIME (port 13) enabled by default; started manually. |
| `TELNET` / `FTP` clients, `SET HOST` | Shipped | Client verbs; IPv4 literal only. |

**Auto-start is not wired at 0.6.** No boot or system-startup procedure invokes
`TCPIP$CONFIG` or `TCPIP$STARTUP`, and no `TCPIP$*` logical is defined by
`SYLOGICALS.COM`. The operator runs the procedures below **manually** each time
they are needed. (Auto-wiring is a kit-install step that is not yet shipped —
see design §6.)

---

## Step 1 — Configure the core environment

Run the configuration procedure as `SYSTEM`. It defines the host and domain
logicals, applies the interface address, and optionally sets a default route
and name server.

Interactive:

```dcl
$ @SYS$MANAGER:TCPIP$CONFIG
```

The menu offers **1** Core environment, **2** Client components, **3** Server
components, **E** Exit. At 0.6 only **Core environment** does anything — the
Client and Server options report "no changes required" and exit.

Parameter-driven (non-interactive), which drives the same core-environment
path:

```dcl
$ @SYS$MANAGER:TCPIP$CONFIG host domain iface addr mask [gateway] [nameserver]
$ ! e.g.:
$ @SYS$MANAGER:TCPIP$CONFIG OVMXNODE example.local SE0 192.168.1.50 255.255.255.0 192.168.1.1 192.168.1.1
```

What the core-environment step does:

1. `DEFINE/SYSTEM/EXECUTIVE TCPIP$INET_HOST` and `TCPIP$INET_DOMAIN`.
2. `TCPIP SET INTERFACE <iface> /HOST=<addr> [/NETWORK_MASK=<mask>]`.
3. Optionally `TCPIP SET ROUTE /DEFAULT /GATEWAY=<gateway>`.
4. Optionally `TCPIP SET NAME_SERVICE /SYSTEM /SERVER=<ns> [/DOMAIN=<domain>]`.
5. `TCPIP SHOW CONFIGURATION` to echo the result.

---

## Step 2 — The `TCPIP$*` logicals (the durable config store)

Host, domain, and address are stored as **executive-mode SYSTEM logicals** in
`LNM$SYSTEM` — this is the authoritative, durable configuration store:

| Logical | Set by | Meaning |
|---------|--------|---------|
| `TCPIP$INET_HOST` | `TCPIP$CONFIG` core env | Local host name. |
| `TCPIP$INET_DOMAIN` | `TCPIP$CONFIG` core env | Local domain name. |
| `TCPIP$INET_HOSTADDR` | `TCPIP SET INTERFACE` | The configured interface IPv4 address. |

Inspect them like any logical:

```dcl
$ SHOW LOGICAL TCPIP$INET_HOST
$ SHOW LOGICAL/TABLE=LNM$SYSTEM TCPIP$INET_*
```

Config databases such as `TCPIP$HOST.DAT`, `TCPIP$INTERFACE.DAT`,
`TCPIP$ROUTE.DAT`, and `TCPIP$NAMESERVICE.DAT` are flat files in the system
directory (written by the `SET` verbs), not logical names.

---

## Step 3 — Interfaces and routes: the `TCPIP` verb

The `TCPIP` management verb (DCL built-in) supports exactly these subcommands at
0.6; anything else returns `%TCPIP-...-IVKEYW`:

```
TCPIP SHOW INTERFACE [/FULL]
TCPIP SHOW ROUTE
TCPIP SHOW HOST
TCPIP SHOW VERSION
TCPIP SHOW CONFIGURATION
TCPIP SET HOST
TCPIP SET NAME_SERVICE /SYSTEM /SERVER=<addr> [/DOMAIN=<name>]
TCPIP SET INTERFACE <name> /HOST=<ipv4> [/NETWORK_MASK=<mask>]
TCPIP SET ROUTE /DEFAULT /GATEWAY=<ipv4>
```

### SHOW INTERFACE

Read-only introspection of the substrate's interfaces, presented with VMS
device names. Linux names are mapped: `eth*`/`ens*`/`enp*` → `SEn`, `lo` →
`LO0`, `wlan*`/`wlp*` → `EWn`, `tun*` → `TNn`. Raw Linux names are never shown.

```dcl
$ TCPIP SHOW INTERFACE
$ TCPIP SHOW INTERFACE /FULL
```

### SET INTERFACE

Applies an address to the substrate interface (via `SIOCSIFADDR` /
`SIOCSIFNETMASK`), persists it, and records `TCPIP$INET_HOSTADDR` in
`LNM$SYSTEM`:

```dcl
$ TCPIP SET INTERFACE SE0 /HOST=192.168.1.50 /NETWORK_MASK=255.255.255.0
```

This requires privilege (effective UID 0); without it you get `%TCPIP-W-PRIVREQ`.
With no executive present it records nothing and reports `%TCPIP-W-NOEXEC`
rather than pretending to succeed.

### Routes and name service

```dcl
$ TCPIP SET ROUTE /DEFAULT /GATEWAY=192.168.1.1
$ TCPIP SET NAME_SERVICE /SYSTEM /SERVER=192.168.1.1 /DOMAIN=example.local
$ TCPIP SHOW ROUTE
```

### Verify the configuration

```dcl
$ TCPIP SHOW CONFIGURATION
$ TCPIP SHOW VERSION
```

`SHOW CONFIGURATION` reads the `TCPIP$INET_*` logicals back from the executive;
with no executive it reports `%TCPIP-W-UNAVAIL` rather than inventing an
address.

---

## Step 4 — PING

`PING` sends real ICMP echo requests over the executive `BGn:` device
(`$ASSIGN TCPIP$DEVICE:` → raw ICMP `$QIO`):

```dcl
$ PING 192.168.1.1
$ PING 192.168.1.1 /COUNT=10
```

**The target must be a dotted-quad IPv4 literal** — name resolution is not
wired at 0.6. If the `BGn:` executive device is unavailable, `PING` reports
`%PING-E-NONET` / `SS$_NOSUCHDEV`. Default count is 4.

---

## Step 5 — The auxiliary server (inetd-equivalent)

`TCPIP$INETD.EXE` is the auxiliary server: it reads the service database, binds
each enabled service's port over the `BGn:` seam, and on an inbound connection
fork/execs the service image with the connection on fd 0/1 (the classic inetd
"wait" model — one connection at a time).

Start it manually after core config:

```dcl
$ @SYS$STARTUP:TCPIP$STARTUP
```

This runs `SYS$SYSTEM:TCPIP$INETD.EXE` against
`SYS$SYSTEM:TCPIP$SERVICE.DAT`. If the image isn't staged you get
`%TCPIP-W-NOINETD`; if the DB is missing, `%TCPIP-W-NOSERVICEDB`. With no
`/dev/vms` the server exits non-zero with `%TCPIP-F-NOSUCHDEV`.

### The service database

`SYS$SYSTEM:TCPIP$SERVICE.DAT` is a plaintext file, one service per line:

```
service-name  port  image  [args]
```

At 0.6 exactly one service is enabled by default:

```
DAYTIME 13 SYS$SYSTEM:TCPIP$DAYTIME.EXE
```

(An SSH line is present but commented out until `VMSSSHD.EXE` ships.) To enable
a service, add or uncomment its line and restart `TCPIP$STARTUP`.

---

## Client utilities

The client verbs connect over `BGn:` and, like `PING`, require IPv4 literals
(no resolver at 0.6):

```dcl
$ TELNET 192.168.1.10
$ FTP 192.168.1.10
$ SET HOST 192.168.1.10
```

---

## Not yet supported at 0.6

These appear in the design (`docs/design-tcpip-services-ovmx.md`) or in OpenVMS
but are **not shipped** at 0.6. Do not rely on them:

- **NIC as a VMS device (`EWA0:`/`EZAn:`).** The NIC is not exposed as a VMS
  device; the bootable image runs with the substrate NIC only. `SHOW DEVICE
  EWA0:` and `$ASSIGN EWA0:` do not work. (compat: `nic-device: absent`.)
- **Name resolution / BIND resolver.** `PING`, `TELNET`, `FTP`, and `SET HOST`
  require dotted-quad IPv4 literals.
- **DHCP / auto-configuration.**
- **Multiple interfaces** and `TCPIP$INET_HOSTADDRn` — a single interface only.
- **Persistent binary service/proxy/network databases** — the service DB is a
  plaintext file; there is no `TCPIP$PROXY`/`TCPIP$NETWORK` binary store.
- **`TCPIP` verb grammar beyond the subcommands listed above** — e.g.
  `SHOW DEVICE_SOCKET`, `SHOW COMMUNICATION`, `SHOW PROTOCOL`, `SET SERVICE`,
  `SET PROTOCOL`, `SET COMMUNICATION` are design-only.
- **SSH / `VMSSSHD.EXE`** — commented out in the service DB; not in the image.
- **Server daemons** (TELNET/FTP/SMTP/NTP/SNMP/LPD servers) — only the TELNET
  and FTP *clients* and the DAYTIME server exist. The wizard's "Server
  components" menu is a no-op.
- **Auto-start integration** — nothing in boot/system startup runs
  `TCPIP$CONFIG` or `TCPIP$STARTUP`, or defines the `TCPIP$*` logicals; run them
  manually.
- **Layered-product kit install** (`OVMX <arch> TCPIP` kit via `PRODUCT
  INSTALL`) — described in the design but not yet shipped as an installable kit.

---

## Quick reference

```dcl
$ @SYS$MANAGER:TCPIP$CONFIG               ! configure host/domain/address
$ TCPIP SET INTERFACE SE0 /HOST=192.168.1.50 /NETWORK_MASK=255.255.255.0
$ TCPIP SET ROUTE /DEFAULT /GATEWAY=192.168.1.1
$ TCPIP SHOW CONFIGURATION                ! verify
$ TCPIP SHOW INTERFACE /FULL
$ PING 192.168.1.1                        ! IPv4 literal only
$ @SYS$STARTUP:TCPIP$STARTUP              ! start the auxiliary server
```
