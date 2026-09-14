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
