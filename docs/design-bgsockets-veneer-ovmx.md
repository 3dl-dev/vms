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

## 8. DECC$SHR's sockets are $QIO on BGn: channels (vms-99f)

**Finding.** DECC$SHR whole-archives musl `libc.a`. Its `socket`, `decc$socket`,
`connect`, `send`, `recv`, `select`, `poll`, `getaddrinfo` and the rest were
musl's: raw Linux socket system calls, plus musl's resolver, which sends DNS over
Linux UDP. A VMS-linked program therefore did host-side Linux networking dressed
up as VMS.

**Rule (Baron, 2026-11-02).** No Linux libc and no Linux descriptor on the I/O
path: every socket call ends in VMS system services on the BG channel, as TCP/IP
Services' DECC$SHR does. An earlier draft of this item materialized the channel
as a vms.ko `[bgconn]` fd and left `read`/`write`/`poll`/`select` as musl calls
on it. That draft was rejected and is not what shipped.

**Design: a socket descriptor names a channel.**
`src/vmstcpip/sockets/ovmx_decc_sockets.c` is linked into DECC$SHR, and
`mk_decc_shr.sh` binds every socket and resolver universal to it, together with
the descriptor calls a program makes on a socket. Every operation is a `$QIO`
function on the channel, issued through `src/libvms/include/bg_qio_func.h`. That
file is the BGn: function map, and `sys$qio` uses the same file.

| Call | $QIO on the channel |
|------|--------------|
| `socket()` | `$ASSIGN TCPIP$DEVICE:` + `IO$_SETMODE` (create) |
| `connect()` | `IO$_ACCESS` (P1 = address) |
| `bind()` / `listen()` | `IO$_SETMODE` (P1 = address / P3 = backlog) |
| `accept()` | `$ASSIGN` a channel + `IO$_ACCESS\|IO$M_ACCEPT` |
| `send`/`write`/`writev`/`sendto`/`sendmsg` | `IO$_WRITEVBLK` |
| `recv`/`read`/`readv`/`recvfrom`/`recvmsg` | `IO$_READVBLK` |
| `shutdown()` | `IO$_DEACCESS` |
| `close()` | `$DASSGN` when the channel's last descriptor goes |
| `setsockopt` / `getsockopt` | `IO$_SETMODE` / `IO$_SENSEMODE` with the P4 option list: a `TCPIP$C_SOCKOPT` / `_TCPOPT` / `_IPOPT` item_list_2 (the vms-a9e7 shape), one entry per option |
| `getsockname` / `getpeername` | `IO$_SENSEMODE`, `IO$K_BG_SENSE_LOCAL` / `_PEER` |
| `select()` / `poll()` | `IO$_SENSEMODE`, `IO$K_BG_SENSE_READY`: the executive's readiness wait |
| `O_NONBLOCK`, `MSG_DONTWAIT`, `FIONBIO` | a zero-timeout readiness sense first; not ready gives `EAGAIN` |
| `dup` / `dup2` / `fcntl(F_DUPFD)` | another descriptor naming the same channel; `O_NONBLOCK` is shared |

The parameter layouts (P3 subfunction codes, the P4 list type and item codes, the 8-byte
address block) are OVMX's own and are labelled as such in `iodef.h` (Rule 8).

**The readiness wait.** `VMS_IOCTL_BG_SELECT` (`vms_ioctl_bg_select` in
`src/kernel-core/vms_bg.c`) takes up to 64 channels, each with the conditions it
waits for, and a timeout. The executive holds a reference on each channel's
socket and asks each socket for its state through `exec_socket_wait_ready`. On
Linux this is the `do_select` pattern on the socket's own `->ops->poll`: the first
pass hangs a wait entry on each socket's wait queue, and the task then sleeps
until a state change wakes it or the timeout runs out. The NetBSD twin re-senses
`soreadable`/`sowritable` on a `kpause` tick. A signal ends the wait with
`SS$_ABORT`, which `select`/`poll` report as `EINTR`. A channel with no socket
gives `SS$_IVCHAN` and `POLLNVAL`. `select` reports that as `EBADF`.

**The descriptor number.** The program still needs a small integer that no file
will also get, so `FD_SET` and `poll` sets work. DECC$SHR reserves the number
with a close-on-exec duplicate of its executive-device descriptor. Nothing is
ever read, written or polled through that descriptor. The audit described
below would count any such call.

**Ordinary descriptors are unchanged.** On a descriptor that is not a socket,
`read`, `write`, `close`, `poll`, `select` and the rest call musl's own entry
point, exactly as before. A `poll`/`select` with no socket in it is musl's.

**Transport.** DECC$SHR is a non-TLS producer and cannot link `vms_kif.c`, whose
`/dev/vms` descriptor is `__thread`. It also has no `--use` of LIBVMS$SHR, so it
cannot call `sys$qiow`. It keeps its own process-wide descriptor instead. It
includes the shared BGn: marshalling (`src/libvmssys/vms_kif_bg_ops.h`, also
included by `vms_kif.c`) and the shared `$QIO` function map
(`bg_qio_func.h`, also included by `sys_qio.c`). The system services therefore
have one definition, used by two images.

**Honest failure.** With no `/dev/vms`, `$ASSIGN` fails `SS$_NOSUCHDEV` and
`socket()` returns -1 with `ENODEV`. Unsupported cases fail with their own errors:
`AF_INET6` and `AF_UNIX` through `socket()` give `EAFNOSUPPORT`, UDP and raw
sockets give `EPROTOTYPE`, `FIONREAD` gives `EOPNOTSUPP`, and a non-socket
descriptor given to a socket call gives `ENOTSOCK`.

**Audit of every DECC$SHR entry that could reach the host network.**

| Entry | Decision |
|-------|----------|
| socket, connect, bind, listen, accept, accept4, send, recv, sendto, recvfrom, sendmsg, recvmsg, shutdown, getsockname, getpeername, setsockopt, getsockopt | `$QIO` on the channel (above) |
| read, write, readv, writev, close, dup, dup2, fcntl, ioctl, poll, select | On a socket: `$QIO` / `$DASSGN` (above). On any other descriptor: musl, unchanged. |
| getaddrinfo, freeaddrinfo, getnameinfo, gethostbyname, gethostbyaddr | Numeric IPv4 only. Names fail `EAI_FAIL`/`NO_RECOVERY` until the BGn: UDP resolver (vms-a9e7). musl's resolver is never called. |
| getservbyname, getservbyport | Return NULL. musl's version reads the host's `/etc/services`, not the node's services database, which is not wired yet. |
| if_nametoindex, if_indextoname, if_nameindex | Fail. musl's version opens a Linux socket and queries the host's interfaces. |
| socketpair | Stays musl: an AF_UNIX pair inside the process. Local IPC, not network. |
| inet_*, hton*/ntoh*, gai_strerror, herror, hstrerror | Stay musl: pure conversions. |
| getprotoby*, getprotoent, getnetby*, gethostent, getservent, set/end*ent | Stay musl: a static table or stubs that touch nothing. |
| gethostname | Stays musl: `uname(2)`, the node's own name. |

**Proof.** `tests/qemu/test_syssvc_decc_socket.c` runs SOCKECHO.EXE
(`tests/qemu/sockecho/`), a BSD-sockets program linked with `LINK.EXE --use
DECC$SHR` only, against a real `/dev/vms` under a ptrace system-call audit.
- Bytes round-trip byte-exact with the suite's own executive listener through
  send/recv, write/read, poll and select. The image also runs its own
  bind/listen/accept leg, with `O_NONBLOCK` (`EAGAIN`), a waited-out `select`
  timeout, a `poll` wake-up and `dup`.
- The descriptor is not a Linux socket.
- The audit counts zero Linux socket-family system calls and zero
  poll/select-family calls. It also counts zero read/write-family calls on a
  socket descriptor or on the executive device. Only `/dev/vms` ioctls carry
  the socket's I/O.
- With an empty `/dev`, `socket()` fails `ENODEV`, and the same counts are still
  zero.

Negative controls: `decc-socket-bound-to-musl` (socket() back to musl),
`decc-socket-read-via-musl` (read() on a socket back to musl's read(2)) and
`bg-select-revents-dropped` (the executive's readiness wait loses its answer).

**Gaps (open).**
- The Alpha DECC$SHR, built by the alpha branch of `mk_decc_shr.sh` from musl-alpha,
  still exports musl's decc$ sockets.
- No UDP sockets.
- No half-close: `shutdown(SHUT_WR)` shuts both directions.
- No `FIONREAD`.
- A socket channel belongs to the process that made it, as a VMS channel does.
  A forked child's calls on an inherited socket fail `EBADF`.
- stdio on a socket (`fdopen`) reaches the descriptor that reserves the number
  and fails. musl's stdio calls the system directly, so DECC$SHR never sees
  the call.
- A `poll`/`select` that mixes sockets with other descriptors alternates a 10 ms
  executive wait with a zero-timeout poll of the others. Terminal readiness
  through the executive is the follow-on.
- (Closed.) `TCPIP SHOW INTERFACE` used a host datagram socket for its interface
  probe, so the VMS-native DCL reported `%TCPIP-E-SOCKERR`. It now `$ASSIGN`s
  `TCPIP$DEVICE:` and issues `IO$_SENSEMODE` with `IO$K_BG_SENSE_IFLIST`
  (`VMS_IOCTL_BG_IFLIST`). With no executive it reports `%TCPIP-E-NOEXEC`.
  `TCPIP SHOW ROUTE` still reads the substrate's `/proc/net/route`.
