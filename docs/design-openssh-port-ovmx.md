# Design: Porting real OpenSSH to OVMX (server + clients)

> **Status:** scope/plan (not implementation). Item: `vms-843` (redirected).
> **Operator ruling (2026-08-14):** SSH is the networking client that matters.
> **Do NOT hand-roll SSH** — crypto + protocol are security-critical. **Port the
> real OpenSSH** (OpenBSD upstream + the portable tree, which has an OpenVMS-port
> lineage via GNV). This supersedes the original `vms-843` body, which scoped a
> minimal from-scratch SSH-2 server; that approach is abandoned.
> **Analysis basis:** `origin/main` at `2270e68a`. All line cites are against
> `origin/main`.

## 0. TL;DR — the make-or-break deps, resolved

| Dep | Status on `origin/main` | Verdict |
|-----|------------------------|---------|
| **Unix PTY** (OpenSSH's `openpty`/ptmx for the shell) | **AVAILABLE.** PID 1 mounts `devpts` on `/dev/pts` at boot (`src/ovmx_init/ovmx_boot_linux.c:69-70`); the current daemon already uses host `openpty()` successfully. | **NOT a blocker.** Guest Linux supplies the pty OpenSSH needs. |
| **Executive VMS terminal device** (TNA0:/RTA0: for `SHOW USERS`/`$GETDVI`/TT:) | **ABSENT.** `vms_devtab.c` creates only `OPA0:` (console). No remote-pty device driver; `vms_ioctl_setterm` records a *name* string, not a device. | **Fidelity prerequisite, parallel — NOT function-blocking.** A working authenticated DCL session does not need it; faithful terminal reporting does. |
| **BGn: server path** (listen/accept for inbound sshd) | **ABSENT.** `vms-527` landed BGn: **client-only** (`vms_kif.h:705-722`: create/setmode/connect/send/recv/deaccess/dassgn); `vms_bg.c` explicitly defers "listen/accept handoff" to Phase 3. | **Hard prerequisite for sshd.** Clients (which only connect) are unblocked. |
| **OpenSSL / libcrypto** | **ABSENT as OVMX image.** On the L1 GNV list (`vms-4fa`), marked **[post-1.0], blocked**. Only a corpus fetch script exists in-tree. | **Do NOT gate on the OVMX image.** Vendor a **cross-built static libcrypto** (LibreSSL preferred) for the musl-static link now. |
| **zlib** | Absent in-tree. | Cross-build static, or build `--without-zlib` for the first cut. Minor. |

**The single most important finding:** the "likely long pole" — a pseudo-terminal
facility — **splits into two different things**, and only the *fidelity* half is
missing. The pty OpenSSH itself consumes (Unix master/slave) is already provided by
the guest kernel and is mounted at boot. So the port can reach a working,
authenticated, interactive DCL session **without building any new executive PTY
facility**, exactly as the current libssh daemon does today. The executive
remote-terminal device (TNA0:/RTA0:) is a real prerequisite for *faithful* VMS
terminal semantics (`SHOW USERS`, `$GETDVI` on the session channel), but it is a
**parallel** workstream that does not block first-light SSH.

---

## 1. Current-state assessment (cited)

`src/vmsssh/` is a **fork-per-connection sshd built on libssh**, plus two
dependency-free glue TUs (`term_map.c`, `cred_drop.c`) and their unit tests.

### 1.1 Transport / KEX / auth / channel — all **libssh**, over a **host TCP socket**
`src/vmsssh/vmssshd.c` (939 lines):
- Binds a **host TCP** listener via libssh, **not** BGn:: `ssh_bind_new()` →
  `ssh_bind_options_set(..., SSH_BIND_OPTIONS_BINDPORT, ...)` →
  `ssh_bind_listen()` → accept loop `ssh_bind_accept()` (`vmssshd.c` main, the
  `ssh_bind_*` block). **The current daemon does not touch the OVMX socket layer
  at all** — it uses libssh's own `socket()`/`bind()`/`listen()`/`accept()`.
- Key exchange: `ssh_handle_key_exchange(session)`.
- Password auth loop: `ssh_message_get()` / `SSH_REQUEST_AUTH` /
  `SSH_AUTH_METHOD_PASSWORD`, validated against SYSUAF (see §1.3).
- Channel + `pty-req` + `shell` + `window-change` handled through the libssh
  message/callback API (`channel_pty_window_change`, the `SSH_CHANNEL_REQUEST_PTY`
  / `SSH_CHANNEL_REQUEST_SHELL` handling).
- Host key: `ssh_pki_generate(SSH_KEYTYPE_RSA, 2048, ...)` self-generated if
  missing.
- Links `ssh` (libssh) and `util` (`openpty`) — `src/vmsssh/CMakeLists.txt`. This
  is precisely what **cannot link under musl-static** for the bootable image (the
  original `vms-843` rationale).

### 1.2 PTY — **host Linux `openpty()`** + a hand-rolled fork
- `#include <pty.h>` (`vmssshd.c:35`), `openpty(&master_fd, &slave_fd, ...)`,
  then `fork()`, child does `setsid()` + `ioctl(slave, TIOCSCTTY, 0)` +
  `dup2(slave, STDIN/OUT/ERR)`. Parent runs a `poll()` relay between the libssh
  channel and the pty master.
- This is a **host pty**, not a VMS terminal device. It works because the guest
  kernel provides `/dev/ptmx` (devpts mounted at boot, §0).

### 1.3 The OVMX-worthy glue — **real, and to be KEPT verbatim in spirit**
This is the part that is genuinely OVMX and is the true work of the port:
- **SYSUAF password auth** — `sysuaf_lookup()` / SYSUAF record; the auth decision
  is against the VMS user authorization file, not `/etc/passwd`.
- **Executive VMS identity** — `vms_kif_setident(username, uic, privs)`
  (`vmssshd.c` child path, ~`:439`) stamps the SSH-authenticated identity into the
  executive so `SHOW PROCESS`/`SHOW SYSTEM` report the real user. Mirrors
  `tools/vms_login.c` (LOGINOUT).
- **Credential drop** — `ovmx_cred_drop_to_uic(uic_group, uic_member, &ovmx_cred_real_syscalls)`
  (`vmssshd.c` ~`:568`, implemented in `cred_drop.c`, unit-tested in
  `tests/vmsssh/test_cred_drop.c`). setgroups→setgid→setuid, ordered, permanent,
  **fail-closed** (`_exit(1)` on failure). This is the LOGINOUT-equivalent drop
  and it **closes `vms-49e`** (the "runs everyone as root" bug) and effectively
  resolves the security concern behind `vms-475`.
- **LOGINOUT → DCL** — `execl(DCL.EXE, "vmsdcl", "--login", "--lgicmd", <LGICMD>)`
  (`vmssshd.c` ~`:585`), honoring the account's SYSUAF LGICMD (or
  `SYS$LOGIN:LOGIN.COM`), with `SYS$LOGIN`/`SYS$LOGIN_DEVICE` established as real
  `LNM$JOB` logicals by DCL's `--login` path (vms-e48).
- **Banner + accounting** — `ovmx_banner_welcome()`, `ovmx_accounting_get_lastlogin`
  / `record_login` (last-interactive-login line).
- **`term_map.c`** — `vmsssh_map_term_to_device_type()` maps the SSH `TERM` to an
  OVMX terminal device-type label (Rule 8/10: an OVMX design choice, not VMS
  behavior). **It currently has NO production caller** — `vms-fb9` removed the
  call because passing a terminal identity down through the environment was a
  facade (a VMS terminal is an executive device, and OVMX creates no remote
  terminal). The unit test still exercises it. Its production home returns when the
  executive can create a real remote-terminal device (§2.2 Prereq B).

### 1.4 What is real vs host-backed vs stubbed
| Concern | State |
|---------|-------|
| SYSUAF password auth | **Real** (VMS UAF). |
| VMS identity in executive (`SHOW PROCESS`) | **Real** (via `/dev/vms`). |
| Credential drop to UIC | **Real, fail-closed** (vms-49e). |
| LOGINOUT → DCL with LGICMD | **Real.** |
| SSH transport / KEX / crypto | **Host-backed** — libssh over a **host TCP** socket, not BGn:. |
| PTY | **Host-backed** — guest Linux `openpty()`, not a VMS terminal device. |
| VMS remote terminal (TNA0:/RTA0:, `SHOW USERS`, `$GETDVI`) | **Absent** — no executive facility; `term_map` output goes nowhere. |
| Reachability | **Unreached** — PID 1 / SYSTARTUP launch no sshd (`vms-475`: `start_sshd` deleted from `ovmx_init.c`). |

`docs/compat/facilities/ssh.yaml` correctly records SSH as **absent**, gated
wholly on the networking lane (`vms-67f`), and flags the (now-fixed) cred-drop gap.

---

## 2. The port plan

Replace the **libssh engine** with **real OpenSSH** (OpenBSD `sshd` + portable
`openbsd-compat`), driving its transport over the **BGn: executive socket**
(`vms-527`), and **keep the OVMX glue** (SYSUAF auth, `setident`, `cred_drop`,
LOGINOUT→DCL, banner/accounting) by hooking it into OpenSSH's session-launch and
auth seams rather than libssh's message loop. Add `ssh`/`scp`/`sftp` as
OVMX-native IMGACT images over the BGn: **client** path.

### 2.1 sshd swap — hook the OVMX glue into OpenSSH's seams
OpenSSH's portable tree is explicitly designed to be reshimmed per platform, which
is exactly how the VSI/HP OpenVMS port was built (GNV; §4). The port replaces
*where OpenSSH gets its OS services*, not the protocol:

- **Socket layer → BGn:.** OpenSSH's listener/accept and its channel I/O read/write
  ultimately bottom out in `socket()`/`bind()`/`listen()`/`accept()`/`read()`/
  `write()`/`shutdown()`. Point these at the OVMX **BSD-socket veneer over BGn:**
  (§2.3 Prereq A) so the inbound TCP connection is an executive-resident socket
  (`SHOW DEVICE BG` is real, cross-process; INV-6 honest `SS$_NOSUCHDEV` if
  `/dev/vms` is absent). This is the concrete meaning of "sshd sits on OVMX's
  socket layer."
- **Auth → SYSUAF.** OpenSSH's password-auth path (`auth-passwd.c` → `getpwnam` +
  `sys_auth_passwd`) is shimmed in `openbsd-compat` to resolve **SYSUAF** (reuse
  the existing `sysuaf_lookup` / auth logic), NOT `/etc/passwd` or PAM. Public-key
  auth is a later increment.
- **Session launch → OVMX LOGINOUT.** OpenSSH's `session.c` `do_exec_pty()` /
  `do_child()` (the "become the user and exec the shell" hook) is replaced with the
  OVMX sequence: `vms_kif_setident()` → PCB init → banner/accounting →
  `ovmx_cred_drop_to_uic()` (fail-closed) → `execl(DCL.EXE, --login --lgicmd ...)`.
  This is the *same* code the current daemon runs; it moves from libssh's callback
  into OpenSSH's session hook. **Keep `cred_drop.c` and the LOGINOUT block
  verbatim** — they are already unit-tested and carry the vms-49e fix.
- **PTY allocation.** OpenSSH's `sshpty.c` `pty_allocate()` uses `openpty()`; on
  OVMX this stays on the guest `/dev/ptmx` (devpts mounted at boot). If/when the
  executive remote-terminal device lands (Prereq B), the session additionally
  registers a TNA0:/RTA0: unit and re-homes `term_map`'s output as its device type.
- **privsep / fork.** OpenSSH forks per connection and runs privilege separation
  (unprivileged child + monitor). Running as a guest-Linux userspace image, real
  `fork()` works. The in-process VMS image-activation model (no-fork; see
  `image-activation-process-model`) governs *DCL activating VMS images*, **not** a
  Unix daemon forking — so OpenSSH's fork/privsep is used unchanged. The one VMS
  activation touch is sshd exec'ing **DCL.EXE** as the session image (fork+exec),
  identical to today.
- **Host key + config.** Ship a real `sshd_config` + host keys under
  `distro/rootfs/etc/ssh/` (already the config home). Host keys generated at first
  boot or provisioned.

### 2.2 Two prerequisites, one parallel-fidelity item
- **Prereq A (function-blocking): BGn: server path** — bind/listen/accept in the
  executive (§2.3).
- **Prereq B (fidelity, parallel): executive remote-terminal device** — a
  TNA0:/RTA0: pseudo-terminal *device* in `vms_devtab.c`'s I/O database, bound to
  the network session, so `SHOW USERS` / `SHOW TERMINAL` / `$GETDVI` on the session
  channel report a real VMS network terminal. This is where `term_map`'s output
  regains a production caller. **Does not block first-light SSH.**
- **Prereq C (build-blocking): vendored static libcrypto** (§3.1).

### 2.3 Clients — `ssh` / `scp` / `sftp` as IMGACT images
- Build OpenSSH's `ssh`, `scp`, `sftp` (+ `sftp-server` subsystem) as OVMX-native
  images (IMGACT-activated), same crypto/compat shims as sshd.
- Clients only **connect** — they ride the **BGn: client path**, which is **already
  done** (`vms-527`). So the clients are **unblocked by Prereq A** and can land
  first. `scp`/`sftp` are non-interactive (no pty needed); the `ssh` client's
  local pty (for `-t`) uses the guest `/dev/ptmx`.

---

## 3. Dependency chain — honest status

### 3.1 OpenSSL / libcrypto — **vendor cross-built static, do NOT wait on the image**
OpenSSH's hard crypto dep. As an **OVMX image**, OpenSSL is on the L1 GNV host-layer
list (`vms-4fa`), which is **`[post-1.0]` and `blocked`** (blocked by `vms-a09`,
`vms-bab`). Nothing OpenSSL-as-image is coming before this port. There is **no
OpenSSL/libcrypto in-tree** today (only `tests/corpus/fetch/openssl-curl-vms.sh`, a
corpus fetch).

**Decision:** link a **cross-built static `libcrypto`** into sshd and the clients
for the musl-static bootable image. **Prefer LibreSSL** — it is OpenBSD's own,
ships alongside OpenSSH-portable, is what upstream targets, and cross-builds cleanly
static against musl; plain OpenSSL is the fallback. This keeps the port off the
critical path of the (post-1.0) OpenSSL-as-image work. Reassess adopting the OVMX
OpenSSL image later, once L1/GNV lands — it is an optimization, not a gate.

### 3.2 PTY facility — **the make-or-break dep, resolved**
Two distinct notions, only the fidelity half missing (see §0 and §1.2/§2.2):
- **Unix pty** (what OpenSSH's `sshpty.c`/`openpty` needs): **PRESENT.** Guest
  Linux `/dev/ptmx`, devpts mounted by PID 1 at boot
  (`ovmx_boot_linux.c:69-70`). The current libssh daemon already relies on it.
  **No new executive PTY facility is required to get a working interactive
  session.**
- **VMS remote-terminal device** (TNA0:/RTA0:, executive I/O database): **ABSENT.**
  `vms_devtab.c` creates only `OPA0:`; `vms_ioctl_setterm` stores a name string in
  `struct vms_proc::terminal` (`vms_internal.h:617`), not a device. Needed for
  faithful `SHOW USERS`/`$GETDVI`/TT: semantics → **Prereq B (parallel)**.

**Verdict:** the pty question does **not** make the port a long pole. First-light
SSH reuses the guest pty exactly as today; the executive remote-terminal device is
a well-scoped, independent fidelity item that can land before we *claim*
terminal-reporting fidelity, not before SSH functions.

### 3.3 zlib — minor
Optional compression. Cross-build static, or first cut `--without-zlib`. Not on the
critical path.

### 3.4 C-RTL / `openbsd-compat` surface — the real shim work
OpenSSH-portable's `openbsd-compat/` already fills BSD-isms on non-BSD hosts. The
OVMX-specific shims:
- **`getpwnam`/`getpwuid` → SYSUAF** (not `/etc/passwd`): the pivot that makes the
  session run as the VMS user. Reuse `sysuaf_lookup`.
- **`setgroups`/`setgid`/`setuid`** — present and already used by `cred_drop.c`.
- **`openpty`** — guest Linux (§3.2).
- **`syslog`/`crypt`/`getentropy`/`arc4random`** — from musl + the vendored
  libcrypto; `getentropy` maps to `/dev/urandom` in the guest.
- **File-naming / paths** — OpenSSH assumes Unix paths; keep it on the guest Linux
  side of the seam (its config/keys are Linux files under `/etc/ssh`), and let the
  VMS-ness appear only at the DCL session boundary — mirroring how the VSI port kept
  OpenSSH Unix-shaped under GNV and put VMS at the LOGINOUT edge.

### 3.5 fork / privsep / image-activation
As §2.1: OpenSSH's fork + privsep run unchanged on the guest kernel; the VMS no-fork
image-activation model is orthogonal (it governs DCL→image, not daemon→fork). The
only VMS activation is sshd→`DCL.EXE`, unchanged from today.

---

## 4. How the real OpenVMS OpenSSH port did it (reference, clean-room)
VSI/HP OpenVMS ships **OpenSSH as part of TCP/IP Services**, ported via **GNV
(GNU-on-VMS)** — the POSIX-ish build/runtime layer on OpenVMS. The port keeps
OpenSSH's `openbsd-compat` layer and adds VMS-specific shims: a **SYSUAF-backed
account lookup** in place of `/etc/passwd`, VMS file-spec handling, and a **VMS
LOGINOUT session launcher** rather than Unix `/bin/login` + shell. OVMX follows the
same shape: OpenSSH stays Unix-shaped under the guest kernel; VMS identity, the
credential drop, and DCL appear at the session-launch edge.

**Sources (public only):** VSI OpenVMS TCP/IP Services SSH documentation
(docs.vmssoftware.com), the GNV project, and the **OpenSSH portable source itself**
(BSD/ISC — OSS, legitimately portable). We read and adapt OpenSSH; the VMS-hosting
shims are our own, informed by public GNV/VSI docs.

## 5. Rule 8 / licensing
- **OpenSSH is BSD/ISC-licensed OSS** (OpenBSD upstream + the portable tree). We may
  **legitimately read, port, and adapt** it. Rule 8's clean-room constraint is about
  **VSI/HPE OpenVMS source/binaries**, which we do **not** touch, disassemble, or
  copy. Porting OSS OpenSSH is exactly what Rule 8 permits.
- **libcrypto:** LibreSSL (ISC/OpenSSL-style) or OpenSSL (Apache-2.0) — both OSS,
  vendorable.
- **OVMX design choices are labeled** as such (Rule 8): the `term_map` `TERM`→device
  mapping (already labeled in `term_map.h`), and any BGn: unit-numbering/QIO-mapping
  choices (already labeled in `vms_bg.h`). No VMS-authentic claim is made for them.

## 6. Sequencing — what blocks what

```
                 ┌─────────────────────────────────────────────┐
Prereq C (build) │ Vendor cross-built static libcrypto          │  (independent; do NOT wait on vms-4fa)
                 │  (LibreSSL preferred)                        │
                 └───────────────┬─────────────────────────────┘
                                 │
Prereq A (func)  ┌───────────────┴─────────────┐   BGn: CLIENT path = DONE (vms-527)
   BGn: SERVER   │ bind/listen/accept in vms.ko │        │
   path          │ + kif + BSD-socket veneer    │        │
                 └───────────────┬──────────────┘        │
                                 │                        │
        ┌────────────────────────┼────────────┐          │
        ▼                        ▼             │          ▼
 ┌──────────────┐     ┌────────────────────┐   │   ┌─────────────────────┐
 │ sshd SWAP    │     │ Prereq B (parallel)│   │   │ ssh/scp/sftp CLIENTS │  (needs only C + BGn: client)
 │ libssh→      │     │ executive remote-  │   │   │ as IMGACT images     │  → CAN LAND FIRST
 │ OpenSSH,     │◄────│ terminal device    │   │   └─────────────────────┘
 │ keep glue,   │ fid │ (TNA0:/RTA0:)      │   │
 │ over BGn:    │     │ → SHOW USERS/$GETDVI│   │
 └──────┬───────┘     └────────────────────┘   │
        │                                       │
        ▼                                       │
 ┌────────────────────────────────────────┐    │
 │ Wire sshd into inetd/aux server (vms-3bf)│   │
 │ + SYSTARTUP. Cred drop already landed    │   │
 │ (vms-49e), so vms-475 sequencing is met. │   │
 └──────────────────────────────────────────┘   │
```

- **Clients first:** they need only Prereq C + the already-done BGn: client path.
- **sshd** needs Prereq A (BGn: server) + Prereq C (libcrypto).
- **Prereq B** is parallel; it upgrades terminal *fidelity* and does not block a
  working session. Do it before claiming `SHOW USERS`/`$GETDVI` terminal fidelity.
- **Wiring/launch** comes last, and is safe because the credential drop already
  landed (vms-49e) — do NOT wire sshd into SYSTARTUP before the drop (it did; good).
- **Non-negotiable (Rule 9 / INV-6):** every network + terminal facility routes
  through `/dev/vms`; absent executive → honest `SS$_NOSUCHDEV`, never a
  per-process fake.
