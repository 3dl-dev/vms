# vms-a2d1 — Is the root→group-0 ACP crutch still load-bearing for x86_64 login?

Measured ground truth. This item's trail had burned two unmeasured hypotheses
(getppid; then a "$CREPRC child never stamps executive identity" read-denial
theory that a VAX probe disproved). This is a boot-on-real-runtime measurement,
crutch-on vs crutch-off, run before any fix.

**Verdict: the crutch IS still load-bearing on x86_64.** Disabling it makes the
boot die fatally before the login prompt. It is NOT excise-able as-is. The
precise gap and the authentic fix are below.

## What the crutch is

`acp_check_access()` (`src/kernel-core/vmsfs_acp.c`) puts an accessor in the VMS
**System** protection category when its UIC group number is `<= MAXSYSGROUP`
(SYSGEN default 8) **or** it holds SYSPRV/BYPASS/READALL:

```c
is_system = (acc_group <= ACP_MAXSYSGROUP) || (privs & ACP_PRV_M_SYSPRV) != 0;
```

The `group <= MAXSYSGROUP` half is faithful VMS (vms-581). The **crutch** is that
OVMX maps Linux root (uid 0/gid 0) to UIC **[0,0]**, and group 0 ≤ 8 — so *any*
root-derived process with no established/continued executive identity lands in
the System category "by luck of the environment" (the phrasing is
`src/ovmx_job_control/ovmx_job_control.c`'s own). On the QEMU/Linux runtime every
boot process is root, so this silently grants System access to processes the
executive never stamped SYSTEM.

## Method

Two temporary instruments in a booting `vms.ko` (removed from the shipped tree;
the diffs are reproduced at the end):

1. `acp_check_access()`: for every access, log `pid / prcnam / username /
   acc-UIC / owner-UIC / prot / want / {SYSPRV,BYPASS,READALL} / group≤8` and the
   grant, and recompute the decision **without** the group≤8 path (`denied_noc`).
   Flag any access that was granted **only** because of group≤8 with no SYSPRV as
   `[ROOT-GROUP0-CRUTCH LOAD-BEARING]`.
2. `vms_proc_register()` (`src/kernel/vms_module.c`): log every registration's
   pid / continue-flag / CAP_SYS_ADMIN / derived UIC / perm_privs.

Boot the real runtime (`distro/Dockerfile.bootable` image, KVM), drive
SYSTEM/MANAGER login to the DCL `$`, force all printk to the serial console with
`ignore_loglevel`. Crutch-OFF build: `is_system` changed to exclude group 0
(`acc_group >= 1 && acc_group <= MAXSYSGROUP`), authentic groups 1..8 and SYSPRV
still qualify.

## Result — crutch ON (shipped behaviour)

x86_64 boots to `Username:`, SYSTEM/MANAGER authenticates to the DCL `$`.

Only **two** processes ever call the ACP on the boot-to-login path:

| pid | what | ACP accesses |
|-----|------|--------------|
| 1 | STARTUP.EXE / PID 1 (mount + stage first-hop images) | 348 — all granted by World:E traversal / World:RE image reads, **none** crutch-dependent |
| 64 | PROVISION.EXE, execl'd in-place into DCL.EXE (runs STARTUP.COM) | 360 — including the crutch-dependent ones below |

Registrations: pid 1, 64, 73 register **fresh** as UIC `[0,0]`, `perm_privs=0x13c00f`
(`ENFORCED = CMKRNL|CMEXEC|SETPRV|WORLD|SYSNAM|GRPNAM|MOUNT` — **no SYSPRV/BYPASS**).
pids 74, 75 register `CONTINUE` as `[1,4]` with `perm_privs=0xffff…ffff` (ALL) —
so `register_continue` and `establish_system` *do* work, but only for those two,
and **neither of them ever touches the ACP**.

Crutch-load-bearing accesses — all by pid 64, `prcnam='(none)'`, `user='(none)'`,
`acc=[0,0]`, `SYSPRV=0`:

```
  1×  prot=ff88 want=1   SYSUAF.DAT read           -> GRANTED [CRUTCH LOAD-BEARING]
 90×  prot=aa00 want=3   system-image read+write   -> GRANTED [CRUTCH LOAD-BEARING]
  1×  prot=ba00 want=3   directory read+write      -> GRANTED [CRUTCH LOAD-BEARING]
```

`ff88` is SYSUAF's protection (S:RWE,O:RWE,G:none,W:none — World-denied). The
process reading it to authenticate SYSTEM is UIC `[0,0]` with **no SYSPRV** — it
gets in *only* because group 0 ≤ MAXSYSGROUP. The measured counterfactual
(`denied_noc`) on that same access is DENIED.

## Result — crutch OFF (group 0 excluded from the System category)

The disk mounts (`%OVMX-I-MOUNTED, system disk DKA0: mounted` — World/traversal
grants are unaffected), STARTUP begins, then:

```
vms-a2d1: ACP access pid=63 acc=[0,0] own=[1,4] prot=ff88 want=1 SYSPRV=0 -> DENIED
vms-a2d1: ACP access pid=63 acc=[0,0] own=[1,4] prot=ba00 want=3 SYSPRV=0 -> DENIED   (×31)
%OVMX-F-EXECINIT, no SYSTEM account in SYS$SYSTEM:SYSUAF.DAT
%OVMX-I-EXECINIT, no session could ever authenticate as SYSTEM
```

Boot **halts fatally before `Username:`**. This is deterministic, not a host-memory
OOM: the disk mount and every World-granted access succeed; only the World-denied
SYSUAF read flips to DENIED, and PROVISION halts on it.

## The precise gap

`src/ovmx_provision/ovmx_provision.c` `main()`:

- Lines 754–769: PROVISION scans SYSUAF (`provision_scan_accounts` →
  `ovmx_sysuaf_enum`) to confirm a SYSTEM account exists, and this scan is
  **deliberately ordered BEFORE `vms_kif_establish_system()`** (its own comment:
  "Deliberately BEFORE identity establishment: a SYSUAF with no SYSTEM account
  must halt before anything prints 'system identity … established'").
- So the one remaining World-denied SYSUAF read runs while PROVISION is still a
  fresh `[0,0]` process with no SYSPRV. On x86_64 it succeeds **only** through
  root→group-0. Crutch off → RMS$_PRV → `provision_halt`.
- The comment at line 772 ("NO SYSUAF READ PRECEDES THIS") is about the *identity*
  read that #278 deleted; the **account-scan** read at 754–769 does precede
  establishment and is the World-denied read that leans on the crutch.

A second, independent lean exists further along the same process: after PROVISION
`execl`s DCL.EXE in-place to run STARTUP.COM, the boot DCL comes up as a **fresh
`[0,0]` registration** (not carrying PROVISION's established `[1,4]`), and its
90 system-image writes (`aa00 want=3`) + directory write also depend on the
crutch. The crutch-off boot halts at the PROVISION scan first, so this second
lean was not independently reached — but it is present in the crutch-on trace and
must be closed too before the crutch can be removed.

## Authentic fix (scoped for follow-up — not landed here)

Two parts; excising the group-0 crutch requires **both**, then a green
boot-to-login proof with the crutch removed:

1. **PROVISION establishes SYSTEM before its SYSUAF scan.** `establish_system()`
   does not itself read SYSUAF, so it can run first; then the account scan reads
   the World-denied SYSUAF as authentic SYSTEM `[1,4]`+SYSPRV, not via root-luck.
   Preserve the "halt before 'identity established' prints" property by ordering
   the *message*, not the privilege.
2. **The STARTUP.COM boot DCL carries PROVISION's established SYSTEM identity**
   across the DCL image activation. Today it re-registers fresh `[0,0]` (the
   `register_continue`/establish path that produced the `[1,4]` CONTINUE rows is
   not applied to the boot DCL). It must run as `[1,4]`+SYSPRV so its system-file
   writes go through the System category authentically.

Only after both land does `is_system` stop depending on group 0 for anything on
the boot-to-login path, and the group-0 exclusion can ship with the boot-to-login
gate still green. Do **not** remove the crutch before then — login dies.

## Reproduction

`vms.ko` instruments (apply to a worktree, rebuild the boot image, boot with the
driver below). ACP instrument in `acp_check_access()` after the `denied` compute:

```c
int sys_by_group  = (acc_group <= ACP_MAXSYSGROUP);
int sys_by_sysprv = (privs & ACP_PRV_M_SYSPRV) != 0;
int is_system_noc = sys_by_sysprv;
unsigned denied_noc = want;
if (is_system_noc) denied_noc &= ~(~(unsigned)(prot & 0xFu) & want);
if (is_owner)      denied_noc &= ~(~(unsigned)((prot >> 4) & 0xFu) & want);
if (is_group)      denied_noc &= ~(~(unsigned)((prot >> 8) & 0xFu) & want);
/* World: */       denied_noc &= ~(~(unsigned)((prot >> 12) & 0xFu) & want);
int crutch = (!denied && denied_noc && sys_by_group && !sys_by_sysprv);
pr_info("vms-a2d1: ACP access pid=%d prcnam='%s' user='%s' acc=[%o,%o] "
        "own=[%o,%o] prot=%04x want=%x SYSPRV=%d sys{grp=%d sysprv=%d} -> %s%s\n",
        proc->linux_pid, proc->prcnam[0]?proc->prcnam:"(none)",
        proc->username[0]?proc->username:"(none)", acc_group, acc_member,
        own_group, own_member, prot, want, sys_by_sysprv, sys_by_group,
        sys_by_sysprv, denied?"DENIED":"GRANTED",
        crutch?" [ROOT-GROUP0-CRUTCH LOAD-BEARING]":"");
```

Crutch-OFF variant: `is_system = (acc_group >= 1 && acc_group <= ACP_MAXSYSGROUP)
|| (privs & ACP_PRV_M_SYSPRV) != 0;`

Boot + drive: `docker run --rm --device /dev/kvm --entrypoint bash <boot-image>`
running a driver that boots `qemu-system-x86_64 -enable-kvm … -append
"console=ttyS0 loglevel=8 ignore_loglevel"`, feeds a CR/second until `Username:`,
sends `SYSTEM` / `MANAGER`, and dumps the console (which carries the `vms-a2d1:`
lines). Write the console to a bind-mounted host path so it survives the
crutch-off boot's fatal halt.
