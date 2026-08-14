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

`src/vmstcpip/sockets/vms_bgsock.h`:

| Call | Maps to |
|------|---------|
| `ovmx_socket(AF_INET, SOCK_STREAM, 0)` | `$ASSIGN TCPIP$DEVICE:` + `IO$_SETMODE` (create executive socket) + a socketpair |
| `ovmx_connect(fd, sockaddr_in)` | `IO$_ACCESS` (connect) + start the pump threads |
| `read()`/`write()`/`poll()` on the fd | pumped to `IO$_READVBLK`/`IO$_WRITEVBLK` |
| `ovmx_send`/`ovmx_recv` | thin wrappers over the fd (completeness) |
| `ovmx_shutdown`/`ovmx_socket_close` | half-close / `IO$_DEACCESS` + `$DASSGN` + close |
| `ovmx_inet_pton`/`ovmx_getaddrinfo_numeric` | numeric IPv4 resolution (no DNS yet) |
| `ovmx_bind`/`ovmx_listen`/`ovmx_accept` | **ENOSYS** until the BGn: server path (`vms-698`) |

**Pollable fds.** `ovmx_socket()` returns an ordinary pollable fd (one end of a
socketpair); two pump threads shuttle bytes between it and the executive BGn:
channel. So the app's ordinary `read()`/`write()`/`poll()`/`select()` work
unchanged — this is what lets a large consumer like OpenSSH use the veneer with
only minimal porting (map `socket`/`connect`/`close` → the `ovmx_*` entries; the
app source is otherwise untouched). Every wire byte still transits `$QIO`.

## 3. Honest failure (Rule 9 / INV-6)

No `/dev/vms` → `$ASSIGN TCPIP$DEVICE:` returns `SS$_NOSUCHDEV` → `ovmx_socket()`
fails `ENODEV`. The veneer **never** falls back to a raw Linux `socket()` that
would connect while sharing nothing with the executive. A silent userspace
fallback is exactly the LARP bug class the authenticity invariants kill.

## 4. Proof (Rule 9) — status: OPEN, a real finding

A QEMU Kernel-Executive proof (`test_syssvc_bgsock_echo`: `ovmx_socket()`+
`ovmx_connect()` to a 127.0.0.1 loopback echo peer, byte-exact round-trip over a
real `/dev/vms`, honest-skip 77 without it, with a paired negctl) was written and
run. **It failed in-guest, and the failure is a real design finding, not a test
bug:**

> The current bridge runs **two pump threads doing concurrent BLOCKING `$QIOW`
> — `IO$_READVBLK` and `IO$_WRITEVBLK` — on the SAME BGn: channel**. In QEMU this
> wedges (the client's `write()` fails / the run times out). The proven raw path
> `test_syssvc_bg_echo` does write-**then**-read *sequentially on one thread*, so
> it never exercises concurrent same-channel QIO. A pollable-fd veneer inherently
> needs both directions live at once, so the bridge must not serialize on one
> blocking-QIO channel.

**Fix direction (next increment), pick one and prove it:**
1. **Async QIO + AST multiplex on one channel** — issue `$QIO` (not `$QIOW`) for
   both directions with completion ASTs / event flags, so a single pump services
   read and write completions without two threads blocking the same channel.
2. **Confirm/enable concurrent read+write in the BGn: driver** (`vms_bg.c`) — if
   the executive already tolerates a simultaneous outstanding read and write IRP
   per channel, the two-thread model can stay; the QEMU proof will say which.

Until this lands, the veneer's client connect/close path and the numeric-IPv4
resolver are the verified surface (compile + host-side honest-skip); the byte
round-trip over `/dev/vms` is the open proof. The `test_syssvc_bgsock_echo` suite
+ its `bgsock-recv-length-zeroed` negctl are **held back** (not landed red) until
the bridge is fixed and the proof is green by SHA.

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
