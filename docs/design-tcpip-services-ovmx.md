# Design — TCP/IP Services for OVMX

**Status:** design / teed for the 1.0 march (new networking lane, parallel to clustering + parity).
**Companion:** `docs/design-decnet-ovmx.md` (shares the networking seam ruling in §2).
**Author of record:** conductor, 2026-08-11. Grounds: OVMX standing rulings + VSI TCP/IP
Services public docs (docs.vmssoftware.com) + a full `origin/main` surface inventory.

---

## 1. What we are building

A faithful **TCP/IP Services for OVMX** layered product: the VMS-authentic userspace that lets
OVMX be configured for, and its programs use, IP networking — the `TCPIP` management control
program, `TCPIP$CONFIG`, the `BGn:`/`$QIO` INET device interface, a BSD sockets API for VMS C
code, `TCPIP$*` system logical names + config databases, and the standard CLI tools/services
(PING, TELNET, FTP, BIND resolver, the auxiliary/inetd server).

The IP stack itself is **Linux's** — we never reimplement TCP/UDP/IP. Faithfulness lives
entirely in the userspace surface that sits on top.

## 2. Seam ruling (standing-ruling-compliant) — shared with DECnet

**Networking is a LAYERED PRODUCT, not a base-executive primitive.** This is faithful to VMS:
TCP/IP Services and DECnet ship as *installable kits* on top of the executive, not as part of
it. The executive supplies the device/`$QIO`/process framework; the IP engine in real VMS is a
loadable driver + ACPs (INETDRIVER, INETACP) — a layered product.

Consequences under the standing rulings:

- **Rule 9 (one runtime).** Engine = *what the Linux kernel provides where it exists*. For IP
  that is the full Linux AF_INET/AF_INET6 stack. No Docker-runtime shim is designed for or
  around; the target is the QEMU/kernel runtime.
- **INV-6 (no silent userspace fallback for an executive facility).** Three touch-points cross
  into **shared/executive state** and MUST be honest — route through `/dev/vms` where the
  substrate exists, else **fail honestly** (`SS$_NOSUCHDEV`/`SS$_IVDEVNAM`), never a per-process
  fake that reports success while sharing nothing:
  1. **`BGn:` device registration** in the executive device namespace (so `$ASSIGN`, `SHOW
     DEVICE BG`, and cross-process channel visibility are real).
  2. **`TCPIP$*` system logical-name tables** (system table = shared state, same substrate the
     executive gap `vms-6b8`/`vms-ln0` is closing).
  3. **Cross-process visibility** of sockets/connections for `TCPIP SHOW DEVICE_SOCKET`.

  This is the same executive-bridge dependency the parity program hit (`vms-a7e`). Where that
  bridge is not yet built, the honest behaviour is a real `SS$_` error, not a fake.
- **Rule 8 (clean-room).** IP is IETF-standard (RFCs) — **no clean-room constraint on the wire.**
  The only faithfulness obligation is the VMS *management + API surface* (command grammar,
  device/QIO semantics, database formats, logical names), all derived from public VSI docs.
- **INV-0 (trademark ceiling).** Brand as **"TCP/IP Services for OVMX"** (OVMX is our mark).
  Do not claim to be VSI/HP TCP/IP Services; badge "OpenVMS-compatible".

## 3. Current state (origin/main inventory, 2026-08-11)

- **Greenfield for VMS programs.** No `BG:` device, no QIO network path, no usable socket API.
  `docs/design-cluster-node.md:121` states plainly: "`$QIO` has no network path".
- **Existing DCL `TCPIP` verb = a management *facade*** (`dcl_cmd_misc.c:903+`): opens a Linux
  `AF_INET/SOCK_DGRAM` socket only for `SIOCGIF*` ioctls, maps ifnames to VMS device names,
  reads/writes `TCPIP$HOST/NAMESERVICE/INTERFACE/ROUTE.DAT`. It configures nothing durable and
  gives VMS code no stack — a fidelity liability of exactly the kind the parity program kills.
  **This design absorbs and replaces it** with a real management plane fronted by the same verb.
- **Unwired SSH.** `src/vmsssh/vmssshd.c` (libssh, → `VMSSSHD.EXE`) exists but PID 1's
  `start_sshd()` was deleted (`ovmx_init.c:677-688`); it is not launched, and there is no NIC to
  reach it on. It re-homes here as a TCP/IP service (§5, Phase 4).
- **Dead wrappers:** `vms_sys_socket*` (`vms_syscall.h:421`), `VMS_AF_INET` (`vms_types.h:375`) —
  no callers. Reusable as the syscall floor of the sockets API.
- **⛔ Runtime has no NIC.** `run-qemu.sh:70` and `Dockerfile.bootable` both pass `-nic none`.
  **Nothing networks until this changes** — Phase 0 below.
- **Reusable pattern:** `src/vmsscs/` already runs raw Ethernet over `AF_PACKET SOCK_RAW`
  (ethertype 0x6007) with the executive device model — the datalink/device idioms transfer.

## 4. Architecture (layers, bottom-up)

```
5. Services / tools   src/vmstcpip/services  PING TELNET FTP resolver auxiliary-server SSH
4. Management plane   src/vmstcpip/mgmt      TCPIP control program, TCPIP$CONFIG, DBs, TCPIP$ lnm
3. Sockets API        src/vmstcpip/socket    BSD socket() veneer for VMS C (RTL), VMS errno/status
2. INET device        src/vmstcpip/inet      BGn: pseudo-device + $QIO IO$_ functions
── VMS device namespace (executive device table, vms_devtab) — reached via /dev/vms transport ──
0. NIC as VMS device  (vms.ko)               EWA0: Ethernet controller  ← backs a virtio netdev
1. Engine             (Linux kernel)         AF_INET/AF_INET6 sockets on that netdev, netlink cfg
```

> **No `/dev/` in the VMS view (operator point, 2026-08-11).** The QEMU virtio NIC surfaces as a
> **VMS Ethernet device `EWA0:`** (or `EZAn:`) in the executive device table (`vms.ko`
> `vms_devtab`, beside `DKA0:`/`DKA100:`), reached by `$ASSIGN`/`$GETDVI` and listed by
> `SHOW DEVICE`. `BGn:` layers over the IP stack bound to that device. `/dev/vms` is only the
> executive *transport* between userspace and `vms.ko`; it is **never** a VMS-visible name. No VMS
> program ever sees a Linux `/dev/` path.

- **L0 NIC as VMS device.** The virtio NIC is registered as `EWA0:` in the executive device table
  (needs the `vms-6b8` device-table substrate; absent it, `SHOW DEVICE` has no `EWA0:` and
  `$ASSIGN` → `SS$_NOSUCHDEV`). This is the honest device face DECnet's circuit also layers over.
- **L1 engine = Linux sockets.** On that NIC's backing netdev, OVMX opens ordinary Linux sockets;
  the kernel does TCP/UDP/IP. Config (interfaces, routes, ARP, DNS) via netlink/`ioctl`.
- **L2 INET device / `BGn:`.** `$ASSIGN` to `TCPIP$DEVICE:` returns a `BGn:` unit backed by a
  Linux socket fd. `$QIO` functions translate faithfully: `IO$_SETMODE|IO$_SETCHAR`
  (bind/listen/socket-options), `IO$_ACCESS` (connect/accept), `IO$_WRITEVBLK`/`IO$_READVBLK`
  (send/recv), `IO$_SENSEMODE` (getsockname/getsockopt), `IO$_DEACCESS` (shutdown/close). This is
  the SRI-QIO / INETDRIVER interface. **Device registration is honest** (executive namespace via
  the `/dev/vms` transport, surfaced as `BGn:`, else `SS$_NOSUCHDEV`).
- **L3 sockets API.** BSD `socket()/bind()/connect()/listen()/accept()/send()/recv()` as the C
  RTL exposes them, mapping to L2 or directly to Linux sockets, with VMS `errno`/status semantics.
  The dead `vms_sys_socket*` wrappers become this layer's floor.
- **L4 management plane** (absorbs the current facade). The `TCPIP` control program grammar
  (`SET/SHOW CONFIGURATION|INTERFACE|ROUTE|HOST|NAME_SERVICE|COMMUNICATION|SERVICE|PROTOCOL`),
  `TCPIP$CONFIG.COM` menu, the config databases (`TCPIP$HOST/ROUTE/SERVICE/NETWORK/PROXY/
  NAMESERVICE.DAT`), and the `TCPIP$*` **system** logical names. Applies config to the Linux
  stack via netlink; persists to the VMS databases; presents VMS semantics and output shapes.
- **L5 services + tools.** The **auxiliary server** (inetd-equivalent: access control + event
  logging + master listener), the service daemons (BIND resolver first, then TELNET/FTP servers,
  later SMTP/NTP/SNMP/LPD), and client tools (`PING`, `TELNET`, `FTP`, `TRACEROUTE`, plus DCL
  `SET HOST/TELNET`). `VMSSSHD.EXE` re-homes here, launched by the auxiliary server.

## 5. Scope & phasing (→ rd children)

| Phase | Outcome (verifiable end state) | Notes / deps |
|---|---|---|
| **0a. NIC infra** | The QEMU VM has a virtio NIC; `run-qemu.sh`/`Dockerfile.bootable` offer user-mode (outbound NAT) + an opt-in **tap/bridge** mode; guest netdev reaches the host. | Dependency-free, first item. Unblocks DECnet's L2 tap + oracle capture. |
| **0b. NIC as VMS device `EWA0:`** | The NIC is registered in the executive device table; `SHOW DEVICE EWA0:`/`$GETDVI`/`$ASSIGN` are real; no `/dev/` in the VMS view. | Needs `vms-6b8` device table. Gates the IP mgmt/socket layers. |
| **1. Management plane real** | `TCPIP SET/SHOW` durably configures the Linux stack via netlink; config DBs + `TCPIP$*` system logicals are honest; `TCPIP$CONFIG` runs. Old facade replaced behind the same verb. | Needs L4 lnm honesty → executive-bridge (`vms-a7e`/`vms-ln0`). |
| **2. INET device + sockets** | A VMS C program `$ASSIGN`s `BGn:` and completes a TCP echo via `$QIO`; the BSD sockets veneer round-trips. `SHOW DEVICE BG` is real. | Core seam. Honest device registration via `/dev/vms`. |
| **3. Client tools** | `PING`, `TELNET`, `FTP` client work from DCL against a real peer. | Depends on Phase 2 (or thin direct-socket for PING). |
| **4. Auxiliary server + services** | Auxiliary server launches BIND resolver + TELNET/FTP servers (+ re-wired `VMSSSHD.EXE`); inbound connect from host succeeds. | Needs tap or hostfwd from Phase 0. |
| **5. e2e QEMU gate** | CI gate: booted VM gets an address, resolves a name, PINGs the gateway, FTP round-trips a file; **honest-degradation** sub-test asserts `SS$_NOSUCHDEV` with no NIC / no `/dev/vms`. | Release proof. |

## 6. Packaging

A **layered-product kit** (`TCPIP$`) installed via the Alpha/PCSI model (`vms-718`):
`TCPIP$STARTUP.COM` invoked from the STARTUP phases (`vms-46c`), gated to **not** announce
running if no NIC is present (no LARP). Kit contents: management images, INET/BG driver-shim,
service daemons, `TCPIP$CONFIG.COM`, DCL tool images, the `TCPIP$*` logical-name and database
templates. Config lives under `TCPIP$ETC:`/`SYS$SYSTEM:TCPIP$*.DAT`.

## 7. Testing & oracle

IETF-standard wire ⇒ **no clean-room constraint**; test against real Linux peers (the QEMU
host, a sidecar container) and loopback. Faithfulness tests assert **command grammar + output
shape + database format** against public VSI TCPIP docs. A real VMS TCPIP node (lab, optional)
is a *behaviour* oracle for command output only, not required to make progress.

## 8. Coordination / lane ownership

New-file lane `src/vmstcpip/**` — disjoint from clustering (`src/vmsscs/**`) and parity
(`src/vmsdcl/**`) except two shared touch-points, both handled append-only / by-sequencing:
the DCL **verb table** (the `TCPIP` verb front-end; one owner per week, append-only) and the
**executive device table** (`src/kernel/**` — Executive seat owns; BGn: registration lands
through the bridge, never a parallel edit). Runtime NIC touches `distro/boot/run-qemu.sh` +
`Dockerfile.bootable` (Phase 0, coordinate with boot owner).

## 9. Open operator calls

1. **Service breadth for 1.0.** Recommend: resolver + TELNET + FTP + SSH in-scope; SMTP/NTP/
   SNMP/NFS/LPD deferred to post-1.0. (Conductor default unless overridden.)
2. **Kit branding** under INV-0 — "TCP/IP Services for OVMX" proposed; confirm.
