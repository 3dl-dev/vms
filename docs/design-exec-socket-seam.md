# `exec_socket_*` — the substrate networking seam (vms-9951, CONVERGE #4)

**Status:** GO taken; seam + both backends BUILT (branch
`work/vms-9951-socket-seam-scope`). Committed: §12 contract `exec_kbackend.h`
(3d1926b3); Linux backend + NetBSD contract decls (303d4680). REMAINING (rides
the post-combine PR, see the execution checklist at the end): the `vms_bg.c` →
kernel-core move, the pollfd rind, the NetBSD twin body + its build wiring, and
the 3-way compile proof. Parent: vms-1d6 (arch-convergence).

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

---

## As-built backends (303d4680)

Two refinements over the plan above, both simplifying the move:

1. **`exec_socket_t` IS the reference-counted holder, not a bare host socket.**
   The plan had `exec_socket_t == struct socket *` with the kref living in
   `vms_bg.c`'s `struct vms_bg_socket`. As built, the kref moved *into the seam*:
   Linux `exec_socket_t == struct exec_socket_holder * { struct socket *sock;
   struct kref kref; }`, and `exec_socket_create` allocates the holder +
   `kref_init`, `exec_socket_get`/`_release` are the refcount, and
   `exec_socket_holder_free` (kref release) does `sock_release`+`kfree`. **Effect
   on the move:** `vms_bg.c`'s entire `struct vms_bg_socket` + `vms_bg_socket_free`
   **disappear** — the channel holds an `exec_socket_t` directly (`ch->bs`), and
   every `kref_get`/`kref_put(&bs->kref, vms_bg_socket_free)` becomes
   `exec_socket_get`/`exec_socket_release`. The seam owns the lifecycle; the
   facility just holds and passes the handle. This makes the move *more* mechanical
   than the plan estimated, not less.
2. **Addresses cross the seam as raw net-order scalars**, never a `struct
   sockaddr` — `exec_socket_connect(s, family, port_be, addr_be)` and
   `exec_socket_getname(s, peer, *family, *port_be, *addr_be)`. The backend builds
   the `sockaddr_in`; no host address type appears in shared core. So `vms_bg.c`'s
   `struct sockaddr_in sa` / `struct sockaddr_storage ss` locals disappear too.

**Linux backend shape (as built, `static inline` in `exec_kbackend_linux.h`):**
chosen over a `vms_socket_linux.c` TU because every op is a thin KPI wrapper (no
uio/objects like NetBSD needs) — the block-device seam split the same way (Linux
inline, NetBSD `.c`). `exec_socket_raw(s)` returns `s->sock` and is the **only**
Linux-only member of the seam — the readiness poll rind's single reach-through.

**`getopt_int` honesty (INV-6), preserved verbatim from `vms_bg.c`:** the backend
reads the live socket state for exactly the OpenSSH integer whitelist —
`SO_KEEPALIVE`←`sock_flag(sk,SOCK_KEEPOPEN)`, `SO_REUSEADDR`←`sk->sk_reuse`,
`SO_ERROR`←`-sock_error(sk)`, `TCP_NODELAY`←`tcp_sk(sk)->nonagle&TCP_NAGLE_OFF`,
`IP_TOS`←`inet_sk(sk)->tos` — and returns an **honest `-ENOPROTOOPT`** (→
`SS$_BADPARAM`) for anything else. Never a faked value. The NetBSD twin must
re-express the same whitelist over `sogetopt`/`sockopt_getint`, honest-fail
identical.

## API-compatibility check (cascade step 1) — **GO, no external breaks**

Question: does adding the `exec_socket_*` seam + moving `vms_bg.c` break any
existing consumer?

- **Seam contract (`exec_kbackend.h` §12): purely additive.** New symbols only;
  no existing `exec_*` signature, type, or macro changed. Every current
  `exec_kbackend` consumer (`vmsfs_acp.c`, `vms_devtab.c`, the lock/EF/AST/arena
  facilities) is untouched — they don't reference `exec_socket_*`. No name
  collision: `exec_socket_*` is new; grep confirms the only prior mention is
  `vms_bg.c`'s own header comment *anticipating* the seam.
- **IOCTL ABI unchanged.** The move does not touch `struct vms_bg_*_args`, the
  `VMS_IOCTL_BG_*` numbers, or the pollfd fd contract (readiness-only anon fd).
  **Userspace is unaffected, and sits two layers above the moved code:** it
  reaches BG through the `vms_kif_bg_*` shim (e.g. `src/libvms/syssvc/sys_assign.c`
  calls `vms_kif_bg_dassgn`, not a raw ioctl), and `src/vmstcpip/sockets/vms_bgsock.c`
  + the OpenSSH poll path (vms-22a) ride that shim — none names the ioctl numbers
  the move preserves, so all keep working byte-for-byte.
- **Internal-only consequences (not breaks):** (a) `vms_bg.c`'s header rationale
  "there is no `exec_socket_*` seam … inventing one would be gold-plating" is now
  **obsolete** and is rewritten by the move; (b) the Linux `src/kernel/Makefile`
  loses `vms_bg.o` and gains `vms_bg_pollfd.o`; kernel-core gains `vms_bg.o`;
  `src/kernel-netbsd/Makefile` gains `vms_socket_netbsd.o`; (c) the NetBSD executive
  gains a `BGn:`-capable seam it does **not yet wire a device to** (runnable =
  vms-024) — it fails honestly (no `TCPIP$DEVICE:` dispatch), never fakes success.

**Verdict: GO.** No API-compatibility blocker; zero external breaking changes; the
cascade proceeds to test-coverage (the 3-way compile proof + the existing bgsock
negctls, which are ABI-level and survive the move unchanged) and this doc (step 3,
done here).

## Post-combine execution checklist (the `vms_bg.c` move — run in one pass)

> **EXECUTED** on post-#803 main (rebased onto f7d59524). All of A–D done in one
> pass; the move landed exactly as planned (the `exec_socket_t`-is-the-holder
> refinement made `struct vms_bg_socket` disappear as predicted). **Proof (E),
> BOTH backends now ground-source proven:**
> - **Linux `vms.ko` GREEN (host-light):** `make` in `src/kernel` compiled
>   `../kernel-core/vms_bg.o` + `vms_bg_pollfd.o`, MODPOST passed, and `vms.ko`
>   linked with **no undefined symbols** — the `exec_socket_*` Linux backend, the
>   core↔rind `vms_bg_ref_socket` boundary, and vms_module.c's dispatch all resolve.
> - **NetBSD/vax twin GREEN (reproduced locally with the exact CI toolchain +
>   pinned NetBSD 10.1 syssrc):** `build-vms-module-vax.sh` compiled
>   `vms_socket_netbsd.c` freestanding for **elf32-vax, -Werror, ILP32
>   width-clean**, emitted an `elf32-vax` object, and relocatable-linked the
>   15-TU module — "ALL PROOFS PASSED". This is the authoritative twin type-check
>   (the amd64 `crosscompile.sh` gate is a narrower pre-ACP subset that, by design,
>   excludes BOTH backend twins — blockdev and socket — so the vax full-module gate
>   is where both twins are proven).
>
> **Finding while proving the vax leg:** the twin first `#include`d
> `<sys/sockopt.h>`, which **does not exist in NetBSD** — `struct sockopt` +
> `sockopt_init/setint/getint/destroy` live in `<sys/socketvar.h>` (already
> included). Every other socket(9)/uio(9) KPI signature was verified against the
> NetBSD 10.1 syssrc oracle and matched. Lesson: an LP64 amd64 gate that omits a
> TU cannot stand in for the ILP32 vax gate that compiles it — ground-source the
> compile, don't infer it from a sibling arch.

Ordered, mechanical, on **post-combine main** after the #803 vmsfs-combine lands
(rebase the branch first; the backends don't touch the deleted vmsfs trees). File
references are to `src/kernel/vms_bg.c` @ 303d4680 unless noted.

**A. Create shared-core `src/kernel-core/vms_bg.c`** (git-mv the file, then edit):
   1. Rewrite the header comment (lines 12–23): drop "there is no `exec_socket_*`
      seam / gold-plating"; state it now rides the seam, host sockets confined to
      the backend, pollfd split to the Linux rind.
   2. Drop the Linux socket includes (lines 47–64: `<linux/net.h>`, `<net/sock.h>`,
      `<net/tcp.h>`, `<net/inet_sock.h>`, `<linux/tcp.h>`, `<linux/in.h>`,
      `<linux/socket.h>`, `<linux/kref.h>`, plus the pollfd-only `<linux/fs.h>`
      `<linux/file.h>` `<linux/poll.h>` `<linux/anon_inodes.h>`). Keep `<linux/
      slab.h>`/`<linux/list.h>`/`<linux/uaccess.h>`/`<linux/atomic.h>` (allocator/
      list/copy/atomic are core-portable via the substrate already) — or, cleaner,
      route through the exec_* equivalents if the other kernel-core TUs do.
      Add `#include "exec_kbackend.h"`.
   3. **Delete** `struct vms_bg_socket` (77–80) and `vms_bg_socket_free` (82–88).
   4. `struct vms_bg_chan.bs` (94): `struct vms_bg_socket *` → `exec_socket_t`.
   5. `bgchan_lookup` (153–163) out-param: `struct socket **sock` → `exec_socket_t
      *sock`; body `*sock = (ch && ch->bs) ? ch->bs->sock : NULL;` → `... ? ch->bs
      : NULL;` (the handle, not `->sock`). Same for every local `struct socket
      *sock` → `exec_socket_t sock` in the eight handlers.
   6. **setmode** (208–263): `sock_create_kern(...)`+`kmalloc(bs)`+`bs->sock=sock`+
      `kref_init` → `exec_socket_t bs; rc = exec_socket_create(&bs);` (holder is
      allocated inside). Race-drop path `sock_release(bs->sock)+kfree(bs)` →
      `exec_socket_release(bs)`.
   7. **connect** (270–304): delete `struct sockaddr_in sa` + its fill; `kernel_
      connect(sock,&sa,...)` → `exec_socket_connect(sock, args.sin_family,
      args.sin_port, args.sin_addr)`.
   8. **send** (309–356): delete `struct msghdr msg; struct kvec vec;` setup;
      `kernel_sendmsg(sock,&msg,&vec,1,a->len)` → `exec_socket_send(sock, a->data,
      a->len)`.
   9. **recv** (363–408): same shape → `exec_socket_recv(sock, a->data, bufsz)`.
   10. **deaccess** (415–438): `kernel_sock_shutdown(sock,SHUT_RDWR)` →
       `exec_socket_shutdown(sock)`.
   11. **dassgn** (447–476) + **release_all** (686–702): `kref_put(&ch->bs->kref,
       vms_bg_socket_free)` → `exec_socket_release(ch->bs)`.
   12. **getname** (549–592): delete `struct sockaddr_storage ss; struct
       sockaddr_in *sin;`; the two `kernel_get{sock,peer}name` + family check +
       field extraction → `rc = exec_socket_getname(sock, args.which,
       &args.sin_family, &args.sin_port, &args.sin_addr);` (backend does the
       AF_INET check, returns `-EAFNOSUPPORT` → `SS$_ABORT`).
   13. **sockopt** (608–678): the whole `set` branch → `exec_socket_setopt_int(
       sock, args.level, args.optname, args.optval)`; the whole `get` whitelist →
       `exec_socket_getopt_int(sock, args.level, args.optname, &args.optval)` with
       the same `rc==0 ? SS$_NORMAL : SS$_BADPARAM` mapping. Delete the `struct
       sock *sk` local.

**B. Extract the Linux-only pollfd rind → new `src/kernel/vms_bg_pollfd.c`:**
   - Move `vms_bg_pollfd_poll` (102–113), `vms_bg_pollfd_release` (115–122),
     `vms_bg_pollfd_fops` (124–129), and `vms_ioctl_bg_pollfd` (485–539) here.
   - This TU keeps the Linux fd includes (`<linux/fs.h>`/`file.h`/`poll.h`/
     `anon_inodes.h`) + `exec_kbackend.h`. It reaches the raw socket via
     `exec_socket_raw(bs)` in `_poll` (`sock->ops->poll`).
   - The channel lookup+ref needs a kernel-core helper (the rind can't walk
     `vms_bg_chan` internals): add `exec_socket_t vms_bg_ref_socket(struct vms_proc
     *proc, uint32_t chan)` to `vms_bg.c` (lookup under `chan_lock` + `exec_socket_
     get`, NULL if no channel/socket); export via `vms_bg.h`. The pollfd's file
     `release` → `exec_socket_release(bs)`.

**C. Backend body + wiring (the FOUR-PLACES trap — miss one, a CI leg reds):**
   - `src/kernel-netbsd/vms_socket_netbsd.c` (NEW): the 10 ops over `socreate`/
     `soconnect`(+`soisconnected` wait)/`sosend`/`soreceive`/`soshutdown`/`soclose`/
     `sockopt_*`, per the table above. **Runnable is vms-024** — for vms-9951 this
     is the contract-only twin: compiles + type-checks, honest-fails where a live
     device isn't wired.
   - Wire the new NetBSD TU in **all four**: (1) `src/kernel-netbsd/Makefile` SRCS;
     (2) `tools/cross-vax/build-vms-module-vax.sh`; (3) `tools/cross-vax/build-devvms-vax.sh`;
     (4) `tests/netbsd/Dockerfile` guest-src. **(4) is edited by the #803 combine —
     that is why C waits for post-combine main.**

**D. Makefiles (Linux):** `src/kernel/Makefile` — remove `vms_bg.o`, add
   `vms_bg_pollfd.o`; kernel-core build — add `vms_bg.o`. (These are the gated
   `src/kernel/Makefile` hunks.)

**E. Proof + cascade:** 3-way compile — Linux `vms.ko` (host-light module build)
   + NetBSD cross-kmod (`build-vms-module-vax.sh`, type-checks the twin) + confirm
   the x86_64 bgsock negctls (`test_bgsock_*`) still pass unchanged (ABI intact).
   Then open the vms-9951 PR referencing this doc; the design-cascade test-coverage
   + doc steps are satisfied by this section + the proof.
