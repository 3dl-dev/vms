# SSH → LOGINOUT/$CREPRC handoff (vms-843a final layer / vms-16b)

Design prep for the last layer of the booted-distro SSH login. **Do not build until
vms-16b is greenlit — it is Baron-reserved (login-stack consolidation + retirement of
the SSH C-reimpl session shim).** This records the grounded design so the build is
fast on greenlight. (Conductor ruling 2026-09-10.)

## What is already PROVEN (the banked milestone)

On a booted OVMX distro (PR #1128, `ssh-boot-e2e`, console via OPA0:):
- The wrapped OpenSSH sshd (`VMSSSHD.EXE`) is ACP-staged from `SYS$SYSTEM:` and
  launched as a **detached daemon** (`sshd -D`, the vms-9cc-proven model) by
  `TCPIP$SSH_STARTUP` (RUN/DETACHED; `__wrap_main` injects `-D -e -f`).
- `sshd -D` **binds :22 and listen/accepts over the executive BGn: seam**
  (`Server listening on 0.0.0.0 port 22.` / `Connection from 10.0.2.2 … on
  10.0.2.15 port 22`).
- **privsep works** (`/etc/passwd` sshd privsep user on the initramfs).
- The inbound session reaches the login.

Transport + auth-reachability + privsep are done. Only the **DCL-session
activation** remains.

## The remaining fatal (why it is held)

The SSH session shim (`third-party/openssh/ovmx/ovmx_sshd_exec.c` `__wrap_execve` +
`src/vmsssh/sshd_auth.c` `ovmx_sshd_fill_passwd`) sets the login user's shell to
`VMS_SYSTEM_DIR/DCL.EXE` and does a **raw `__real_execve("/vms/…/DCL.EXE", …)`**.
On a booted distro that path is **ODS-2/ACP-only, not on the initramfs Linux VFS**,
so sshd's shell-existence `stat()` fails: *"User SYSTEM not allowed because shell
/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE does not exist"*. This is the **same class as
vms-21b's raw-execv bug**, now for DCL activation — and DCL is a VMS-native
`PT_INTERP=IMGACT` image (multi-image activation: DCL + IMGACT + shareables).

Staging the DCL chain on the initramfs (the KE-test's approach) is **rejected** —
it is exactly the raw-execve-from-the-Linux-VFS anti-pattern vms-21b retired.

## The faithful fix — hand off to LOGINOUT via $CREPRC (the existing primitive)

DECnet SET HOST already does this, and there is a shared primitive to reuse:
`src/vmsdecnet/cterm/dnet_cterm_host.c`
```
inbound connection (validated)
  -> ovmx_vterm_create()                         # executive mints a virtual terminal RTAn:
  -> $CREPRC(LOGINOUT.EXE, RTAn:, PRC$M_INTER|PRC$M_LOGINOUT)   # uic=0, prvadr=NULL
  -> LOGINOUT authenticates on that terminal, re-personas the process, activates DCL
```
`$CREPRC` (`src/libvms/syssvc/sys_process.c`, `starlet.h`) creates the process the
VMS way (executive-resident), `creprc_bind_terminal` binds the connection to the
virtual terminal, and `PRC$M_LOGINOUT` starts LOGINOUT with no identity — it acquires
one only after its own SYSUAF authentication. The daemon never holds/forges a
credential. `tests/integration/test_creprc_session_primitive.sh` gates the primitive.

So the SSH session should NOT raw-execve DCL (nor shim-IMGACT it): it should mint a
virtual terminal for the SSH connection (the accepted BGn:/materialized fd) and
`$CREPRC(LOGINOUT.EXE, <vterm>, PRC$M_INTER|PRC$M_LOGINOUT)`, exactly as console
login (JOB_CONTROL → LOGINOUT) and DECnet SET HOST do. This unifies console + DECnet
+ SSH on one LOGINOUT/$CREPRC primitive and retires the SSH C-reimpl session shim
(the raw-execve DCL activation, the LOGINOUT-equivalent cred-drop in `cred_drop.c`,
the `sshd_session.c`/`ovmx_sshd_exec.c` shims).

## The one open design question — AUTH (SSH already authed; DECnet's LOGINOUT auths)

DECnet SET HOST has **no transport auth**: LOGINOUT does all the auth over the
terminal. SSH is different — the SSH protocol **requires** auth before a session
channel opens, and vms-9cc already built + proved sshd doing **in-protocol SYSUAF/
Purdy password auth**. So the fork:

- **Option A — sshd authenticates; LOGINOUT trusts it (network-login mode).** Keep
  the proven vms-9cc in-protocol SYSUAF auth. `$CREPRC(LOGINOUT, <ssh-vterm>,
  PRC$M_INTER|PRC$M_LOGINOUT + a pre-authenticated/network flag)` passing the
  SSH-authenticated username; LOGINOUT runs a **network-login mode** that trusts the
  established identity (no re-challenge — SSH already consumed the auth exchange and
  authed against the *same* SYSUAF/Purdy authority), then stamps identity + activates
  DCL + session setup. **Requires a new LOGINOUT network-login mode** — OVMX has none
  today (`tools/vms_login.c:115`: "OVMX has no batch/network login"). Lower risk
  (builds on proven auth); reuses the $CREPRC(LOGINOUT) primitive; retires the shim's
  DCL-activation but keeps the sshd SYSUAF-auth wrap.
- **Option B — LOGINOUT does ALL the auth (DECnet-identical).** sshd drops its
  in-protocol SYSUAF auth; SSH keyboard-interactive is bridged to LOGINOUT's
  challenge over the vterm, so LOGINOUT is the single auth authority for console +
  DECnet + SSH and the daemon never holds a credential (the DECnet safety property).
  Most unified + retires the most shim (including the sshd auth wrap), but a bigger
  sshd auth re-architecture (keyboard-interactive → LOGINOUT bridge) and diverges
  from the vms-9cc-proven in-protocol auth.

**Recommendation:** A for the first vms-16b increment (keep proven auth, add a
LOGINOUT network-login mode, reuse $CREPRC(LOGINOUT,vterm), retire the DCL-activation
shim), with B as the fuller-consolidation follow-on if Baron wants LOGINOUT as the
sole auth authority. Either way the mechanism is `ovmx_vterm_create` +
`$CREPRC(LOGINOUT, vterm, PRC$M_INTER|PRC$M_LOGINOUT)`, and the SSH e2e proof is
unchanged: an inbound `ssh SYSTEM@` lands a real DCL `$` (`OVMX_DCL_LANDED_843a`).

## Build checklist (on greenlight)
1. Mint a virtual terminal for the SSH connection (reuse `ovmx_vterm_create` /
   the BGn:-fd → RTAn: bind used by DECnet).
2. Replace the shim's raw-execve-DCL with `$CREPRC(LOGINOUT.EXE, <vterm>,
   PRC$M_INTER|PRC$M_LOGINOUT)` (+ the network/pre-authed path for Option A).
3. (Option A) add the LOGINOUT network-login mode (`tools/vms_login.c`): trust a
   pre-established identity, skip the password read, stamp + start_session.
4. Retire the SSH C-reimpl session shim (vms-16b): DCL-activation, cred_drop
   LOGINOUT-equivalent, sshd_session/ovmx_sshd_exec — replaced by LOGINOUT.
5. Keep the SSH overlay/posture (shipped SSH-off) + the OPA0: diagnostic; the
   `ssh-boot-e2e` goes green when the inbound login lands DCL.
</content>

---

## GROUNDED VERIFICATION + refined build (2026-09-12, vms-16b greenlit)

vms-16b is DECIDED (retire the SSH C-reimpl second login stack; route SSH through
the SAME LOGINOUT/$CREPRC primitive console login + DECnet SET HOST/CTERM use).
Conductor endorsed **Option A** for this increment. Re-verified the primitive
against origin/main by reading the code (not a summary):

- **The primitive EXISTS and is proven.** `src/vmsdecnet/cterm/dnet_cterm_host.c`
  (`dnet_cterm_host_open_desc`) does exactly: `ovmx_vterm_create(devnam,&master_fd)`
  (line 121) → set `master_fd` `O_NONBLOCK` → `sys$creprc(&pid, LOGINOUT, RTAn:,
  RTAn:, RTAn:, NULL,NULL,NULL, 0,0,0, PRC$M_INTER|PRC$M_LOGINOUT)` (line 151) →
  pump the network ⇄ `master_fd` (`dnet_cterm_host_read`/`_write`). `ovmx_vterm_create`
  is implemented at `src/libvms/syssvc/sys_vterm.c:55`; `sys$creprc` honours the
  terminal descriptors + `PRC$M_LOGINOUT` (`src/libvms/syssvc/sys_process.c:843`,
  `creprc_bind_terminal`). Gated by `tests/integration/test_creprc_session_primitive.sh`.
  (A prior exploration summary claimed this did not exist — that was wrong; the code
  is on origin/main.)

- **The build is LOCALIZED to the session shim.** sshd's session child already has
  fd 0/1 wired to the SSH channel (a Linux pty slave for interactive, or the
  materialized BGn: socket via `ovmx_materialize_fd`). So the handoff replaces the
  body of `third-party/openssh/ovmx/ovmx_sshd_exec.c` `__wrap_execve` (the DCL branch,
  line 42-63): instead of `__real_execve(DCL,…)`, the session child
  `ovmx_vterm_create()`s a terminal, `sys$creprc(LOGINOUT, <vterm>×3,
  PRC$M_INTER|PRC$M_LOGINOUT)`, then select()-pumps fd0→master_fd and master_fd→fd1
  until EOF, then `_exit`s the child's status. This is the DECnet daemon loop with
  the "network side" = the SSH-provided fd. It RETIRES the raw-execve DCL activation
  (the vms-16b target) and the `pw_shell=DCL.EXE` / `ovmx_sshd_dcl_login_argv` shim.

- **The one open question — how LOGINOUT gets the SSH-authed identity (Option A).**
  LOGINOUT (`tools/vms_login.c:908`) ignores argv and is purely interactive
  (username/password prompt). `sys$creprc` passes an IMAGE, not argv, so the
  authenticated username cannot ride argv. Two faithful mechanisms, pick one:
    1. **Executive-conveyed (recommended):** the SSH shim, post-auth, stamps the
       authenticated identity onto the minted vterm / created process via the
       executive the same way console login's identity flows (a per-session
       executive attribute or a SYSTEM logical scoped to the created PID), and
       LOGINOUT gains a **network-login mode** that, when that attribute is present,
       trusts it, SKIPS the password read, does the SYSUAF lookup for that username,
       stamps + `start_session` + activates DCL. No credential is forged (SSH already
       consumed the auth exchange against the SAME SYSUAF/Purdy authority).
    2. **LOGINOUT-does-auth (Option B, deferred):** pass no identity (DECnet-identical,
       `uic=0`), let LOGINOUT challenge over the vterm; the SSH keyboard-interactive
       exchange is bridged to LOGINOUT's prompt. More consolidation, bigger sshd
       auth re-architecture, non-standard SSH shape. Follow-on.
  Recommendation stands: mechanism (1) for this increment. The net-new code is a
  LOGINOUT network-login mode (`tools/vms_login.c`, ~the console_login path minus the
  password read) + the shim's vterm/creprc/pump body — both bounded, both testable.

- **Proof (unchanged reap bar):** `tests/qemu/test_ssh_boot_e2e.sh` — an inbound
  `ssh SYSTEM@` (pw MANAGER) lands a real DCL `$` and echoes `OVMX_DCL_LANDED_843a`;
  wrong password never reaches DCL. Iterate on the heavy rail like the daytime e2e.
  Shipped stays SSH-off (posture guard). No compat status flip without this e2e green
  by SHA (enforcing e2e = implemented; the register SSH rows stay honest until then).
