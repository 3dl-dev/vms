# OpenSSH sshd → VMS auth/session glue (OVMX) — 3c implementation design

> **Scope (rd vms-0cd, RUNG 3 step 3c).** How the ported OpenSSH **sshd**
> authenticates against **SYSUAF** and launches a **LOGINOUT→DCL** session on the
> OVMX guest, reusing the already-unit-tested `src/vmsssh/` glue. This is the
> auth/session half of the sshd port; the **transport** half (sshd's listen/accept
> over BGn: via `--wrap`) is separate and BLOCKS on executive BG-channel
> fork-inheritance (RUNG 3 step 3b, `test_syssvc_bg_fork_inherit`, owned by the
> ACP/executive seat). This doc is transport-independent: it survives whatever
> shape the fork feature + the sshd `--wrap` build take.
>
> Builds on `docs/design-openssh-port-ovmx.md` §2.1 (the seam names); this is the
> concrete implementation design (the shim TUs, exact call sites, reuse map,
> tests, build wiring).

## Principle

The OVMX-worthy logic already exists and is unit-tested. **No new auth/session
policy is written** — the same SYSUAF/LOGINOUT flow the console login (`tools/
vms_login.c`) and the old libssh `vmssshd.c` run is transplanted VERBATIM into
OpenSSH's two porting seams. The only genuinely new code is two thin OpenSSH shim
bodies and their placement; everything they call is a landed, tested primitive.

INV-6 / Rule 9 throughout: an absent or refusing executive **denies** the session
(fail-closed) — never a local-privileged fallback (the vms-6ae / vms-49e fixes are
in the glue and move with it).

## A. AUTH seam → SYSUAF (`sys_auth_passwd`)

OpenSSH portable dispatches password auth through the porting hook
`int sys_auth_passwd(struct ssh *ssh, const char *password)` (built by defining
`CUSTOM_SYS_AUTH_PASSWD`; the account is `ssh->authctxt->pw`, populated earlier by
`getpwnam`). Contract: **return 1 = accept, 0 = reject**. This is the single clean
hook — no PAM, no BSD-auth conversation state (design-openssh-port-ovmx.md §2.1:
"NOT /etc/passwd or PAM").

**Shim body** (new TU `ovmx/ovmx_sshd_auth.c`, compiled into sshd):
```
int sys_auth_passwd(struct ssh *ssh, const char *password) {
    const char *user = ssh->authctxt->user;   /* already SYSUAF-upcased by getpwnam shim */
    sysuaf_record_t rec; uint32_t rms_st;
    if (sysuaf_lookup_st(user, &rec, &rms_st) != <success>) return 0;   /* RNF etc. -> reject */
    if (!sysuaf_authenticate(&rec, password)) return 0;                 /* Purdy verify (NO SHA-256) */
    if (!sysuaf_login_check(&rec, /*flags*/)) return 0;                 /* DISUSER/DISACNT/expired/captive */
    return 1;
}
```
Reuse (all landed + tested): `sysuaf_lookup_st`/`sysuaf_lookup` (`sysuaf.h:272`),
`sysuaf_authenticate` (Purdy, `sysuaf.h:291` / `sysuaf.c:319`), and the login-flag
enforcement helper (`sysuaf.h:294`) — exactly the trio `vms_login.c:633,662,669`
runs. The password compare is the same one the old libssh loop used
(`vmssshd.c:248,250`).

**`getpwnam`/`getpwuid` → SYSUAF** (design §2.1 line 132, §3.4 line 218): the
account lookup must resolve SYSUAF, not `/etc/passwd`. Implement as an
`openbsd-compat` override (or a `--wrap=getpwnam`/`getpwuid` at link, consistent
with the transport `--wrap` model) that fills a `struct passwd` from
`sysuaf_lookup` — `pw_name` = upcased username, `pw_uid/pw_gid` from the record's
UIC `[group,member]`, `pw_dir` from the SYSUAF default device+directory,
`pw_shell` = the DCL path (so any residual shell-exec path still points at DCL).
This is the pivot that makes the session run as the VMS user (§3.4).

Public-key auth is a later increment (design §2.1); password/keyboard-interactive
is 3c.

## B. SESSION seam → LOGINOUT/DCL (`do_child`)

After auth, OpenSSH `session.c` runs `do_authenticated → do_exec_pty()/
do_exec_no_pty() → do_child(ssh, s, command)` — the "become the user and exec the
shell" point (it drops creds via `do_setusercontext`/`permanently_set_uid`, sets
the env, and `execve`s `pw->pw_shell`). OVMX **replaces that shell exec** with the
ordered LOGINOUT sequence, transplanted verbatim from `vmssshd.c`'s child
(design §2.1 lines 136-142: "Keep cred_drop.c and the LOGINOUT block verbatim").

**Ordered sequence** (new TU `ovmx/ovmx_sshd_session.c`, called from the
`do_child` hook, order is LOAD-BEARING — mirrors `vms_login.c` and `vmssshd.c`):
1. `ovmx_ssh_establish_identity(rec.username, uic, sysuaf_record_privileges(&rec), &ovmx_ident_real_syscalls, &st)` — executive `vms_kif_setident` (must precede the drop; needs SETPRV registration). Even status → `%OVMX-F-NOIDENT` + `_exit(1)`, NO shell (`vmssshd.c:473`; guard `test_ssh_ident.c`).
2. `vms_pcb_set_identity(getpid(), uic, username, "")` + `vms_pcb_set_default_dir(...)` (`vmssshd.c:497-498`).
3. banner + accounting (`vmssshd.c:568,583`).
4. `ovmx_cred_drop_to_uic(rec.uic_group, rec.uic_member, &ovmx_cred_real_syscalls)` — Linux setgroups/setgid/setuid, ordered + verified + fail-closed (`vmssshd.c:612`; guard `test_cred_drop.c`; vms-49e).
5. `sysuaf_login_command_file(&rec, lgicmd, ...)` then `execl(SYS$SYSTEM:DCL.EXE, "vmsdcl", "--login", "--lgicmd", lgicmd, NULL)` (`vmssshd.c:628-629`) — the LOGINOUT→DCL session that replaces `/bin/sh`.

PTY stays OpenSSH's own `sshpty.c` `pty_allocate()`/`openpty()` on the guest
`/dev/ptmx` (design §2.1 lines 145-148) — the guest kernel provides real PTYs, so
no VMS PTY device is needed for 3c. `term_map.c` has no production caller (the
env-passing facade was removed, vms-fb9) — leave it out until a real
remote-terminal device exists (Prereq B).

## Reuse map (nothing reinvented)

| Need | Landed primitive | Cite |
|---|---|---|
| SYSUAF record read | `sysuaf_lookup` / `sysuaf_lookup_st` | `sysuaf.h:272` |
| Password verify (Purdy) | `sysuaf_authenticate` | `sysuaf.h:291`, `sysuaf.c:319` |
| Login-flag enforcement | login-check helper | `sysuaf.h:294` |
| Record privileges | `sysuaf_record_privileges` | `sysuaf.h:228` |
| Executive identity | `ovmx_ssh_establish_identity` → `vms_kif_setident` | `ssh_ident.h:64`, `vms_kif.h:113` |
| Credential drop | `ovmx_cred_drop_to_uic` | `cred_drop.h:65` |
| LGICMD resolution | `sysuaf_login_command_file` | `sysuaf.h:310` |
| DCL login exec | `execl(DCL.EXE, "vmsdcl","--login","--lgicmd",…)` | `vms_login.c:491`, `vmssshd.c:628` |

The console login (`vms_login.c`) is the working reference for the whole flow.

## Build wiring

- Two new shim TUs `third-party/openssh/ovmx/ovmx_sshd_{auth,session}.c` compiled
  with OpenSSH's own CFLAGS/CPPFLAGS (like the wrap TU) into sshd; the sshd link
  gains the OVMX veneer/sysuaf/libvms objects (the same freestanding subset the
  client build stages, plus `src/vmsssh/{ssh_ident,cred_drop}.c` and the SYSUAF/
  RMS objects). `-DCUSTOM_SYS_AUTH_PASSWD` selects the auth hook.
- The `getpwnam`/`getpwuid`→SYSUAF override: openbsd-compat replacement or
  `-Wl,--wrap=getpwnam -Wl,--wrap=getpwuid` (consistent with the transport wrap).
- **The sshd `--wrap` transport build LANDED (RUNG 3, vms-0cd / PR #823).** Its
  realized shape, which 3c's hooks link into:
  - The server wrap set (`socket`/`connect`/`read`/`write`/… **plus**
    `bind`/`listen`/`accept`/`accept4`) lives in the SAME `ovmx_ssh_wrap.c` as the
    client set, but the four listener wraps and their `__real_*` externs are gated
    behind **`OVMX_WRAP_SERVER`**. This is load-bearing: a link that does not pass
    `-Wl,--wrap=bind …` (the client link) would otherwise leave `__real_bind`
    undefined. **Contract: a shared `--wrap` TU must gate each `__wrap_X`/`__real_X`
    to the binaries whose link actually passes `--wrap=X`.** Compile the object
    twice — `-DOVMX_WRAP` (client) and `-DOVMX_WRAP -DOVMX_WRAP_SERVER` (server).
  - `sshd` (9.8+) re-execs `sshd-session` from its **compiled `--libexecdir`**
    (a `config.h` `#define`, no runtime override), and the accepted connection must
    reach that child — so `sshd-session` is the binary that must carry the wrap
    (the harness applies the wrap set via LIBS-global, covering sshd + sshd-session
    + sshd-auth). Because the baked libexecdir can hold only ONE `sshd-session`
    variant, `build-ssh-harness.sh` configures **twice** into distinct
    `--libexecdir` trees: `/ovmxssh/libexec` (stock `sshd-session`, client-wrap KEX
    proof) and `/ovmxsshsrv/libexec` (wrapped `sshd-session`, server proof).
  - The accepted channel survives sshd's `fork()`+`exec()` of `sshd-session`
    because the executive inherits the BG channel by number via `real_parent`
    (#815) and the veneer handle is self-describing across exec (#822).
- This doc's 3c hooks compile independently; they RUN once linked into the wrapped
  sshd-session of the server tree.

## Tests

- The glue is ALREADY proven, independent of libssh/live `/dev/vms`:
  `test_ssh_ident.c` (identity fail-closed: refuse → no session) and
  `test_cred_drop.c` (drop ordered/verified/fail-closed). Moving the caller from
  libssh's callback to OpenSSH's `do_child` needs no re-test of the glue itself —
  only the new call site.
- **3c-specific:** a focused unit test of the `sys_auth_passwd` shim against a
  SYSUAF fixture (accept a valid Purdy password, reject a wrong one, reject a
  DISUSER/expired account, reject when the executive is absent) — standalone,
  no transport needed.
- **Full authenticated e2e** (a stock/host ssh client authenticates a SYSUAF user
  over BGn: and lands in DCL) is RUNG 3 step 3d — it needs the transport + fork
  feature, so it gates on those, not on this doc.

## Dependency summary

3c (this design) is transport-independent and rework-safe. Sequencing:
1. **3b (blocking, executive seat):** BG-channel fork/exec inheritance
   (`test_syssvc_bg_fork_inherit` greens).
2. **sshd `--wrap` transport build** (deferred): sshd listens/accepts over BGn:.
3. **3c (this doc):** the auth/session shims — implement once (2) exists.
4. **3d:** SYSTARTUP `RUN/DETACHED` launch + the full authenticated e2e.
