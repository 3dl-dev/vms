# Design: OVMX BSD-sockets RTL veneer over BGn:

> **Status:** increment 1 landed (`work/vms-bgsock-veneer`). Prerequisite for the
> OpenSSH clients (`vms-22a`) and the networking lane (`vms-67f`).
> Source: `src/vmstcpip/sockets/vms_bgsock.{c,h}`.

## 1. Why — the missing middle layer

An application (ssh, telnet, any TCP client) opens a socket to a remote IP:port
and lets the stack route; it **never touches the transport interface/device**.
So the correct OVMX layering is:

```
app  →  socket()/connect()   [STANDARD BSD sockets — app speaks ONLY this]
      →  OVMX BSD-sockets RTL veneer over BGn:   ← THIS component (DECC$SOCKET-equiv)
           →  $QIO to BGn:  →  vms.ko  →  host kernel TCP/IP  →  interface
```

BGn: (`vms-527`) is the executive-resident INET **device**: a program reaches it
the ordinary VMS way — `$ASSIGN TCPIP$DEVICE:` then `$QIO` (`IO$_SETMODE` create /
`IO$_ACCESS` connect / `IO$_READVBLK` / `IO$_WRITEVBLK` / `IO$_DEACCESS`), and the
socket lives in the executive over the host kernel's in-kernel socket API. But an
*application* must not speak that — it speaks sockets. The **veneer** is the layer
that translates standard BSD sockets calls into those `$QIO`-to-BGn: ops. This is
the OpenVMS TCP/IP Services sockets-library model (DECC$SOCKET): the socket API
the C RTL exposes, backed by the executive INET device. `tcpip_client.h`
(`vms-dbb`) already shows the exact `$QIO` ops the veneer drives underneath.

> A prior idea shimmed OpenSSH's `sshconnect.c` to speak `$QIO` directly. That
> was the **wrong layer** — it made the app talk to the transport device rather
> than a socket — and is discarded. Apps use sockets; the veneer does the rest.

## 2. API (increment 1 — client path, IPv4)

`src/vmstcpip/sockets/vms_bgsock.h` — each sockets call is ONE `$QIO` op:

| Call | Maps to |
|------|---------|
| `ovmx_socket(AF_INET, SOCK_STREAM, 0)` | `$ASSIGN TCPIP$DEVICE:` + `IO$_SETMODE` (create executive socket); returns an OVMX handle |
| `ovmx_connect(s, sockaddr_in)` | `IO$_ACCESS` (connect) |
| `ovmx_send(s, ...)` / `ovmx_recv(s, ...)` | one BLOCKING `IO$_WRITEVBLK` / `IO$_READVBLK` |
| `ovmx_shutdown` / `ovmx_socket_close` | `IO$_DEACCESS` / `IO$_DEACCESS` + `$DASSGN` |
| `ovmx_inet_pton` / `ovmx_getaddrinfo_numeric` | numeric IPv4 resolution (no DNS yet) |
| `ovmx_bind` / `ovmx_listen` / `ovmx_accept` | **ENOSYS** until the BGn: server path (`vms-698`) |

**Full-duplex, simple blocking semantics.** `ovmx_send`/`ovmx_recv` each do one
blocking `$QIO`; a recv and a send may be outstanding at once on one handle from
two threads (§4). `ovmx_socket()` returns an **OVMX handle** (offset
`OVMX_BGSOCK_BASE`, never a libc fd), used only with the `ovmx_*` calls — a
**pollable-fd** form for OpenSSH's `poll()`/`select()` is the next increment (§4).

## 3. Honest failure (Rule 9 / INV-6)

No `/dev/vms` → `$ASSIGN TCPIP$DEVICE:` returns `SS$_NOSUCHDEV` → `ovmx_socket()`
fails `ENODEV`. The veneer **never** falls back to a raw Linux `socket()` that
would connect while sharing nothing with the executive. A silent userspace
fallback is exactly the LARP bug class the authenticity invariants kill.

## 4. Root cause of the first wedge, and the fix

A first bridge exposed a *pollable fd* via a socketpair + two pump threads
(one `IO$_WRITEVBLK`, one `IO$_READVBLK`). It wedged in-guest. Investigation
(conductor step 1) traced the whole `$QIO` path and found **the executive fully
supports a blocking read and a blocking write outstanding at once on one
channel** — nothing serializes them:

- **kernel dispatch** — `vms_dev_ioctl` is `unlocked_ioctl` (concurrent per-fd);
  it takes no lock.
- **`vms_bg.c`** — `bgchan_lookup` lifts the socket pointer out from under
  `proc->chan_lock` and **drops the lock** before the sleeping
  `kernel_recvmsg`/`kernel_sendmsg`.
- **`sys_qio.c`** — `qio_bg_op` holds no lock across `vms_kif_bg_send/recv`.
- The proc is shared by `current->tgid` (all threads), `REGISTER` is idempotent,
  the kif args are stack-local, and the host kernel socket is inherently
  full-duplex.

So **`vms_bg.c` did not need fixing** — the wedge was in the veneer's own
socketpair/pump lifecycle. The fix removes that machinery: the veneer now issues
**simple blocking `$QIO` directly** (`ovmx_send`→`IO$_WRITEVBLK`,
`ovmx_recv`→`IO$_READVBLK`). Full-duplex is inherent — two threads may hold a
send and a recv outstanding at once.

`ovmx_socket()` returns an **OVMX handle** (offset `OVMX_BGSOCK_BASE`, never a
libc fd), used only with the `ovmx_*` calls. A **pollable-fd form** (for
OpenSSH's `poll()`/`select()` multiplexing) is the next increment — an async
`$QIO` + AST veneer poll, or a fixed fd bridge.

## 4b. Proof (Rule 9) — GREEN

`tests/qemu/test_syssvc_bgsock_echo.c` — a **reader thread blocks in
`ovmx_recv()`** on the connection while the main thread `ovmx_send()`s on the
**same handle** (concurrent `IO$_READVBLK` + `IO$_WRITEVBLK`), against a real
`/dev/vms`, and the echoed reply is asserted **byte-exact**; honest-skip (77)
without the executive; the numeric-IPv4 resolver is checked on both branches.
Negative control `bgsock-recv-length-zeroed` (`facility_defects.sh`, floor
102→103) zeroes `ovmx_recv()`'s returned count so only the byte-exact assertion
reddens.

## 5. Scope / follow-on

- **This increment:** client path (connect/send/recv/close/shutdown) + numeric
  IPv4, over loopback. Server path (bind/listen/accept) is honest `ENOSYS` until
  the BGn: server path (`vms-698`) lands. DNS is a later phase.
- **Rides on this:** OpenSSH links the veneer and activates as the OVMX IMGACT
  image using standard `socket()/connect()` (`vms-22a`); telnet/ftp can move off
  the raw `tcpip_client.h` `$QIO` onto the veneer.

## 6. Clean-room (Rule 8)

The BSD sockets API is POSIX. The VMS-facing surface the veneer speaks
(`TCPIP$DEVICE:`, the QIO function map, the 8-byte BGn: sockaddr) is from public
VSI OpenVMS TCP/IP Services docs + the already-landed BGn: driver — never VSI/HPE
source. The veneer is labeled an OVMX design choice; no VMS-authentic byte-layout
claim is made for it.
