# Nothing in OVMX runs as substrate root (rd vms-8e6)

## The rule

Baron, 2026-10-10: root is a construct of the substrate kernel. A process that
runs as root is like a user binary loaded as a kernel driver. So in OVMX:

- Every userspace process runs under an unprivileged substrate identity. This
  applies to STARTUP after its bootstrap, SYSTARTUP, JOB_CONTROL, LOGINOUT,
  every session, every server and every utility.
- A process's power comes only from VMS privileges, and the executive checks
  them against the caller's own PCB (vms_prot.h).
- An operation that today needs root is redesigned as an executive service
  behind a VMS privilege.

The trust boundary: substrate root is at kernel level, the same as the
operator who loads the executive. OVMX defends against non-root processes.
It cannot defend against root, so nothing in OVMX may *be* root.

## Measuring it (vms-251b)

`SYS$SYSTEM:ROOTAUDIT` (`tools/vms_rootaudit.c`) reads the substrate's own
process list, not the executive's: Linux `/proc`, NetBSD `kern.proc2`.

- It names every process other than a kernel thread that has uid 0 (real,
  effective, saved or fs) or, on Linux, any effective capability.
- The shared DCL acceptance battery runs it as the unprivileged GUEST session,
  after STARTUP and two logins, on x86_64, Alpha and VAX.
- The result is compared against `substrate_root_known` in
  `tests/qemu/lib/dcl_acceptance_battery.sh`. Each entry names the rd item
  that removes it. Both an unlisted root process and a stale entry fail the
  gate, so the list can only shrink.
- vms-251b is done when the list is empty.
- aarch64 has no booted acceptance run yet, so it is not measured.

## Substrate identity (vms-ac48, vms-137e)

- **One substrate uid per VMS process,** taken from a dedicated range
  (`OVMX_SUBSTRATE_UID_BASE`, never 0). The executive assigns it when it
  creates the process: the creator-driven `CREPRC_PCB` of vms-9f32 PR2. On
  Linux this uses `prepare_creds`/`commit_creds` on the new task; on NetBSD,
  `kauth_cred` on the new proc.
  - The uid is *not* derived from the UIC.
  - Because each VMS process has its own uid, the substrate's same-uid powers
    (ptrace, kill, /proc/<pid>/mem) never reach another VMS process.
- **VMS identity lives only in the executive:** UIC, privileges and rights.
  Every place that reads `getuid()`/`getgid()` as a UIC instead reads the PCB:
  - `sys_security.c get_uic`
  - `vms_devtab.c caller_uic`
  - `decnetd.c`
  - RMS file protection
- **LOGINOUT** keeps its `setuid()` until creator-driven creation (#1591 PR2)
  lands. After that it calls no `setuid()`: the executive re-personas the
  process's VMS identity (SETIDENT), and its substrate uid stays the one the
  executive gave it. Per-user image staging (`/run/ovmx-boot/<uid>`) becomes
  executive-owned staging.
- **STARTUP (PID 1)** does only what the substrate kernel requires of init,
  and does it as root:
  - mount proc/sys/dev/tmp
  - NetBSD: mknod `/dev/vms`
  - the executive attach and the system-disk mount
  - the staging tmpfs

  Then the executive gives it an unprivileged substrate identity, before it
  runs anything VMS (PROVISION, STARTUP.COM, SYSTARTUP).
- **The first VMS identity** (SYSTEM) is granted by the executive to the
  substrate's init process by construction, not because it holds a
  capability (vms-5df4).

## Root needs that become executive services

| Today (root/capability) | Becomes | Item |
|---|---|---|
| `capable(CAP_*)` / kauth gates in vms.ko and the NetBSD module | `vms_prot_require_priv` on the PCB | vms-5df4 |
| DECNETD raw datalink (`CAP_NET_RAW`, bpf) and its root checks | executive L2 channel (PHY_IO), already on Alpha | vms-ef02 |
| `reboot(2)` at shutdown | executive shutdown service (CMKRNL) | vms-137e |
| `sethostname(2)` after bootstrap | done in bootstrap, or executive (SYSNAM) | vms-137e |
| LOGINOUT `setuid()`/`chown()` of staging dirs | executive persona + executive-owned staging | vms-ac48 |
| TCPIP `SET INTERFACE/ROUTE` (`geteuid()==0`, NET_ADMIN) | executive network-config service (privilege-gated) | vms-ef02 |
