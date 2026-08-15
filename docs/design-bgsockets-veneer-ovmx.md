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

## 4. Root cause of the wedge, and the fix

The first bridge (a socketpair + two pump threads exposing a pollable fd) wedged;
the second (direct blocking `sys$qiow`) failed one assertion reproducibly — **the
send on the main thread passed, the recv on a reader thread failed**. That tell
located the real cause, and it is **not** executive serialization:

- The executive fully supports a blocking read and a blocking write outstanding
  at once on one channel — nothing holds a lock across the socket op (kernel
  `unlocked_ioctl`; `vms_bg.c` drops `proc->chan_lock` before the sleeping
  `kernel_recvmsg`/`kernel_sendmsg`; `sys_qio.c` holds no lock across the kif
  call), and the host kernel socket is full-duplex. So `vms_bg.c` did **not**
  need fixing.
- **The userspace channel table is per-thread.** `src/vmsprocess/vms_pcb.c` keeps
  the PCB pointer in `__thread current_pcb`, and `vms_pcb_init()` gives each
  thread its **own** PCB with its **own** channel table. So the sys$ channel the
  main thread `$ASSIGN`ed does not exist in a reader thread's PCB — its
  `sys$qiow(recv)` cannot find the socket, while the main thread's `sys$qiow(send)`
  finds it. A socket is a *process* resource (SSH reads and writes it from
  different threads), so a per-thread channel table cannot back multi-threaded
  sockets.

**Fix (this increment):** the veneer addresses the **executive** BG channel
directly via the `vms_kif_bg_*` layer. The executive channel **is** process-wide
— `vms.ko` keys the BG channel list on `current->tgid` (shared by all threads) —
so every thread operates on the one connection. It is the same executive-resident
socket and the same `IO$_SETMODE`/`IO$_ACCESS`/`IO$_WRITEVBLK`/`IO$_READVBLK`/
`IO$_DEACCESS` ops; no userspace socket stack; honest `SS$_NOSUCHDEV`→`ENODEV`
without `/dev/vms`.

> **Finding (filed):** making the userspace PCB's channel table *process-wide*
> (so the public `sys$assign`/`sys$qiow` path itself works across threads, as VMS
> channels do) is the VMS-faithful long-term fix. It has a broad blast radius
> across every sys$ service that reads `vms_pcb_get()`, so it is out of scope for
> this increment; the veneer's kif-direct path is the correct, localized answer
> for now.

## 4b. Proof (Rule 9) — full-duplex, GREEN

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

## 7. Pollable-fd increment (vms-22a) — select()-able sockets

OpenSSH's event loop (`clientloop`/`serverloop`) does `poll()`/`select()` on the
connection fd. The veneer's kif-direct handles are not Linux fds, so the executive
now exposes a **real readiness fd** per BG channel:

- **`vms_bg.c` (`VMS_IOCTL_BG_POLLFD`)** hands userspace an **anon-inode fd** whose
  `.poll` **delegates to the executive socket's own `->poll`** — registering the
  socket's wait queue, so `poll()`/`select()` blocks and wakes on the socket's
  *true* readiness and returns its real `EPOLLIN`/`EPOLLOUT` mask. The fd is
  **readiness-only**: no read/write file-ops, so data still moves solely through
  `IO$_READVBLK`/`IO$_WRITEVBLK` and the socket stays executive-resident. The host
  socket is held by a small **refcounted holder** (`struct vms_bg_socket`, kref),
  referenced by both the channel and the poll fd, so it outlives a poll fd still
  open at `$DASSGN`. New kif entry `vms_kif_bg_pollfd` (manifest
  `libvmssys_shr.vec`, append-only); veneer entry `ovmx_pollfd(s)` returns the fd.
- Because `.poll` reflects the real socket state, `poll()` composes transparently
  with OpenSSH's multi-fd event loop — **no OpenSSH poll shim needed**.

**Proof (Rule 9, GREEN):** `tests/qemu/test_syssvc_bgsock_poll.c` — obtain the
readiness fd, confirm `poll()` reports **not readable before any data** (times
out), `ovmx_send()`, then `poll()` **blocks until the loopback peer's echo makes
the fd readable**, and `ovmx_recv()` reads it byte-exact; honest-skip 77 without
`/dev/vms`. Negctl `bgsock-poll-always-ready` (`facility_defects.sh`, floor
103→104) makes `.poll` report readable unconditionally so `poll()` fires before
data.

With this, the veneer surfaces standard `socket()/connect()/send()/recv()` **and**
a `poll()`-able fd — the full plumbing OpenSSH needs. Next: link the vendored
`ssh` against the veneer (`#563`).

## 6. Clean-room (Rule 8)

The BSD sockets API is POSIX. The VMS-facing surface the veneer speaks
(`TCPIP$DEVICE:`, the QIO function map, the 8-byte BGn: sockaddr) is from public
VSI OpenVMS TCP/IP Services docs + the already-landed BGn: driver — never VSI/HPE
source. The veneer is labeled an OVMX design choice; no VMS-authentic byte-layout
claim is made for it.
