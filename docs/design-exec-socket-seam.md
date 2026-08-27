# `exec_socket_*` — the substrate networking seam (vms-9951, CONVERGE #4)

**Status:** scope/design only. No implementation yet — this record exists so the
operator/conductor can make a go/no-go call before the design-change cascade.
Parent: vms-1d6 (arch-convergence).

## Recommendation (go/no-go)

**GREEN, and the same size class as the block-device seam — IF scoped
conservatively:** land the `exec_socket_*` seam + the (near-mechanical) Linux
refactor + a NetBSD backend that is *contract-only, type-checked, never run*,
exactly as the `exec_blockdev_*` NetBSD twin is today. Keep
`VMS_IOCTL_BG_POLLFD` a Linux-only rind over the seam.

**It becomes 2–3× bigger — split it out — if a *runnable* NetBSD in-executive
`BGn:` is in scope.** That pulls in the asynchronous `soconnect` wait, `uio`
plumbing for `sosend`/`soreceive`, the `TCPIP$DEVICE:`/`vms_netbsd.c` dispatch
glue, and a QEMU integration proof. Recommend a **separate follow-on rd item**
for runnable NetBSD networking; do not fold it into the seam increment.

## Why converge

`src/kernel/vms_bg.c` (~702 lines, the `BGn:` INET sockets facility) is a
Linux **kernel-module** TU using the Linux in-kernel socket API (`kernel_*`).
It has **no NetBSD twin** — VAX/NetBSD has no in-executive network at all. It is
the last executive facility that never got a substrate seam (its own header
comment says so). Folding it behind `exec_socket_*` — mirroring `exec_blockdev_*`
— moves ~640 lines of VMS/BG bookkeeping to shared-core and confines the ~40–60
lines of host-socket calls to a per-substrate backend.

## The template it mirrors: `exec_blockdev_*`

- **Seam contract** in `src/kernel-core/exec_kbackend.h`; substrate bound by
  `-DOVMX_KBACKEND_{LINUX,NETBSD}` selecting `exec_kbackend_linux.h` vs
  `../kernel-netbsd/exec_kbackend_netbsd.h`.
- **Shared consumer** `src/kernel-core/vmsfs_acp.c` calls
  `exec_blockdev_read_block`/`_write_block` with zero `#if`.
- **Linux backend** = `static inline` in `exec_kbackend_linux.h` (bio).
- **NetBSD backend** = a standalone `.c` TU `src/kernel-netbsd/vms_blockdev_netbsd.c`
  (`bread(9)`/`getblk`/`bwrite`) listed in `src/kernel-netbsd/Makefile` SRCS.
- **Lesson:** a facility needing real kernel calls ships as a per-substrate `.c`
  TU, not header inlines. The socket backend follows the `.c`-TU model.

## Proposed `exec_socket_*` seam

Opaque `exec_socket_t` (Linux `struct socket *`; NetBSD `struct socket *`).

| Seam fn | Linux | NetBSD |
|---|---|---|
| `exec_socket_create` | `sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, out)` | `socreate(AF_INET, out, SOCK_STREAM, IPPROTO_TCP, curlwp, NULL)` |
| `exec_socket_release` | `sock_release` | `soclose` |
| `exec_socket_connect` | `kernel_connect(s, sa, len, 0)` | `soconnect(s, sa, curlwp)` **+ sleep until `soisconnected`** |
| `exec_socket_send` | `kernel_sendmsg` (kvec) | build `uio(UIO_SYSSPACE)` → `sosend(s,NULL,&uio,NULL,NULL,0,curlwp)` |
| `exec_socket_recv` | `kernel_recvmsg` | build `uio` → `soreceive(s,NULL,&uio,NULL,NULL,&flags)` |
| `exec_socket_shutdown` | `kernel_sock_shutdown(s, SHUT_RDWR)` | `soshutdown(s, SHUT_RDWR)` |
| `exec_socket_getname` | `kernel_get{sock,peer}name` | `pr_usrreqs` PRU_SOCKADDR/PEERADDR |
| `exec_socket_setopt_int` | `sock_setsockopt`/`ops->setsockopt` (KERNEL_SOCKPTR) | `sockopt_setint` + `sosetopt` |
| `exec_socket_getopt_int` | the raw-`struct sock` whitelist, moved to backend | `sockopt_getint` + `sogetopt` |

Deferred (server path, `bgsock` returns ENOSYS today) — declare **contract-only**:
`exec_socket_bind`/`_listen`/`_accept` → Linux `kernel_bind/listen/accept`,
NetBSD `sobind/solisten/soaccept`.

**Shared-core move:** `src/kernel/vms_bg.c` → `src/kernel-core/vms_bg.c`, keeping
all bookkeeping (`struct vms_bg_chan`, the kref `struct vms_bg_socket` now holding
an `exec_socket_t`, `bgchan_lookup`, unit numbering, all ten `vms_ioctl_bg_*`
handlers, `vms_bg_release_all`) and calling `exec_socket_*` instead of `kernel_*`
(~50 call-site edits). Backends: Linux inlines/`vms_socket_linux.c`; new
`src/kernel-netbsd/vms_socket_netbsd.c` in SRCS.

## What CANNOT cleanly move (stays Linux-only in `src/kernel/`)

1. **`getsockopt` whitelist** — reads raw Linux internals (`tcp_sk(sk)->nonagle`,
   `inet_sk(sk)->tos`, `sk_reuse`, `SOCK_KEEPOPEN`, `sock_error(sk)`). The
   *logic* becomes backend code (`exec_socket_getopt_int`), re-expressed honestly
   per substrate — NetBSD `sogetopt` returns these directly. Small but real; the
   "honest, not faked" behavior must be preserved on both sides.
2. **`VMS_IOCTL_BG_POLLFD`** — `anon_inode_getfile`/`fd_install`/`EPOLLIN|OUT`/
   `->poll` is pure Linux fd machinery with **no NetBSD analogue** (NetBSD uses
   kqueue). Keep it a Linux rind over the seam (the poll fd holds an
   `exec_socket_t`). **Load-bearing for OpenSSH (vms-22a)** — flag explicitly so
   NetBSD `BGn:` is never assumed feature-complete without a kqueue design.

## Effort

~600–800 net LOC across ~7 files: seam contract ~120L; the `vms_bg.c`
shared-core move is mostly a move + mechanical substitution; Linux backend
~120–180L; NetBSD backend `vms_socket_netbsd.c` ~250–350L (the majority — `uio`
construction, the `soconnect`→`soisconnected` wait, sockaddr extraction, sockopt
objects) **only if runnable**; contract-only NetBSD is far smaller.

## Risks

1. **NetBSD in-kernel socket from a loadable module.** Precedent exists
   (`sys/kern/subr_tftproot.c` drives `socreate`/`soclose` with `curlwp`), and
   `vms_bg` always runs in process/ioctl context (may sleep) — so no
   interrupt-context problem. But a *runnable* backend must be correct, not just
   compile.
2. **`soconnect` is async** (unlike blocking `kernel_connect(…,0)`) — the
   connect-wait loop on `soisconnected` is the single most error-prone piece and
   has no Linux counterpart.
3. **`sosend`/`soreceive` `uio` setup** — per-call boilerplate; recv-0-is-EOF and
   blocking semantics must match the current `kernel_recvmsg` byte-stream contract.
4. **No portable readiness/poll seam** (see pollfd above) → NetBSD `BGn:` is not
   feature-complete for SSH without kqueue work.
5. **No async requirement this increment** — the client path is synchronous
   blocking `$QIOW`; no AST/event-flag upcall needed now (good).

## Next step

Operator/conductor go/no-go on the conservative scope. If GO: this is a
design-change (kernel-module interface) → the standard cascade (API-compat,
test-coverage, docs) applies to the seam increment; the runnable NetBSD backend
is a separate rd item.
