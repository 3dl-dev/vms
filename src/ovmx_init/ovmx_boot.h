/*
 * ovmx_boot.h - the OVMX boot-plumbing substrate seam (rd vms-28f,
 * epic vms-8e8; design record docs/design-p4-netbsd-vax-boot.md, GAP-C).
 *
 * This is the ONE header src/ovmx_init/ovmx_init.c includes for the host-OS
 * boot primitives PID 1 needs from the substrate it runs on. It declares an
 * abstract vocabulary -- load an executive/filesystem kernel module, bring up
 * the base kernel filesystems, resolve and mount the system disk, open the
 * executive device, start the console-log bridge, and power the machine off
 * -- whose concrete realization is selected at BUILD time from a per-substrate
 * backend translation unit:
 *
 *   Linux   (src/ovmx_init/ovmx_boot_linux.c)   -> finit_module(2) / mount(2) /
 *                                                  reboot(RB_POWER_OFF) /
 *                                                  /dev/kmsg / /dev/vda /dev/vms
 *   NetBSD  (src/ovmx_init/ovmx_boot_netbsd.c, vms-f2e -- NOT in this landing)
 *                                               -> modctl(2) / NetBSD mount(2) /
 *                                                  reboot(RB_HALT|RB_POWERDOWN) /
 *                                                  /dev/klog / /dev/wd0 /dev/vms
 *
 * The point of the seam is that on Linux every op maps to EXACTLY the syscall
 * ovmx_init.c makes today, so promoting PID 1 onto it is a behaviour-preserving
 * refactor (rd vms-28f): identical syscalls, identical args/flags, identical
 * order. The SAME ovmx_init.c then compiles against the NetBSD backend without
 * a single `#ifdef __linux__` fork of the boot logic (INV-DRIFT). This mirrors,
 * userspace-side, the shape src/kernel-core/exec_kbackend.h + its per-substrate
 * backend already established for the executive.
 *
 * WHAT IS AND IS NOT BEHIND THIS SEAM. The boot *sequence and policy* -- what
 * mounts, in what order, which module loads, whether a load failure is fatal,
 * the mount-or-halt decision, the installed-volume gate, running STARTUP.COM --
 * stays in ovmx_init.c, expressed in portable POSIX (mkdir/stat/fork/execl/
 * waitpid/sethostname/pause) and calls to these ops. Only the *how* -- the
 * substrate-specific syscall, device path, or filesystem type -- lives behind
 * the seam. Two peer boot modules keep their OWN substrate readers and are NOT
 * folded in here (each gets its NetBSD twin under vms-f2e alongside this one):
 *   - sysboot.c owns the boot-flag register (Linux: /proc/cmdline). It is a
 *     self-contained "SYSBOOT>" module with its own abstraction; ovmx_init.c
 *     calls it directly, not through this seam.
 *   - opcom_kmsg.c owns the /dev/kmsg record reader; this seam only SELECTS it,
 *     through ovmx_boot_start_console_log_bridge(), so ovmx_init.c names no
 *     console-log-sink primitive itself.
 *
 * Substrate-neutral by construction: this header pulls in NO Linux <linux/...>
 * headers, <sys/mount.h>, <sys/reboot.h>, or <sys/syscall.h>. Only <stddef.h> for
 * size_t. The contract is chosen so each named op maps cleanly to NetBSD --
 * modctl(2), NetBSD mount(2), reboot(2), and the klog device all fit it.
 *
 * FAIL-HONEST CONTRACT ON EVERY BACKEND (INV-6 / CLAUDE.md Rule 9). This holds
 * on ALL substrates, not just Linux, and is part of the contract every future
 * backend inherits: an op reports the host's REAL result and NEVER fakes
 * success. ovmx_boot_open_executive() returns the descriptor of the REAL
 * executive device or a negative error -- it must never return a made-up
 * descriptor that does not name the executive (executive_attach() halts PID 1
 * on its error branch, so a faked descriptor would boot straight past a missing
 * executive -- the exact silent-fallback defect Rule 9 forbids). ovmx_boot_
 * load_module()/mount_system_disk() likewise return the host's real load/mount
 * result and never no-op to a bogus success. The Rule 9 gate
 * (tests/integration/test_runtime_target.sh) enforces this on the Linux backend
 * -- check 3b-backend verifies ovmx_boot_open_executive() really opens the
 * executive device -- and each new backend must extend that gate with the same
 * property for its own device path (vms-f2e for NetBSD).
 *
 * Clean-room (CLAUDE.md Rule 8): this contract and ovmx_init's boot logic are
 * OVMX's own code; each backend maps it to PUBLIC, documented host-OS syscalls
 * only. No Linux, NetBSD, or VSI/HPE source or binary is copied.
 */
#ifndef OVMX_BOOT_H
#define OVMX_BOOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ovmx_boot_kernel_filesystems_mounted - nonzero iff the substrate's base
 * kernel filesystems are already up (so PID 1 is running inside an
 * already-booted userland, not on a fresh kernel that still needs its
 * plumbing built). ovmx_init.c gates bare_metal_init() on getpid()==1 AND
 * this returning zero.
 *
 * Linux: stat("/proc/version") == 0 (procfs mounted => plumbing already up).
 * NetBSD: the equivalent probe of the base mount set (vms-f2e).
 */
int ovmx_boot_kernel_filesystems_mounted(void);

/*
 * ovmx_boot_mount_kernel_filesystems - bring up the base kernel/pseudo
 * filesystems the boot needs before it can read boot flags, reach device
 * nodes, or give sessions ptys and shared memory. Best-effort, exactly as
 * today: individual mounts are not error-checked (a missing pseudo-fs
 * surfaces later as the concrete failure that depends on it). Returns 0.
 *
 * Linux backend mounts, in this order: proc on /proc, sysfs on /sys,
 * devtmpfs on /dev, tmpfs on /tmp, devpts on /dev/pts, tmpfs on /dev/shm
 * (creating /dev/pts and /dev/shm first) -- byte-for-byte the sequence
 * bare_metal_init() ran inline before this seam existed.
 */
int ovmx_boot_mount_kernel_filesystems(void);

/*
 * ovmx_boot_start_console_log_bridge - start the substrate's kernel-log ->
 * SYS$MANAGER:OPERATOR.LOG bridge (vms-32a). Best-effort and non-blocking: a
 * bridge that will not start must never fail or delay boot (the fail-honest
 * gate is the executive, not this operator-visibility aid).
 *
 * Linux backend forwards to opcom_kmsg_start() (the /dev/kmsg reader thread).
 * NetBSD: the /dev/klog twin (vms-f2e).
 */
void ovmx_boot_start_console_log_bridge(void);

/*
 * ovmx_boot_mute_kernel_console - lower the substrate kernel's OWN console
 * log level so routine module printk (KERN_INFO and below) stops reaching
 * the shared boot console, before ANY OVMX kernel module loads (vms-300).
 * QEMU's serial console is BOTH the kernel's own console AND ovmx_init's
 * stdout: vms.ko/vmsfs.ko log their lifecycle at KERN_INFO via pr_info
 * (src/kernel/vms_module.c etc.), and without this, that raw kernel dmesg
 * interleaves with -- and is indistinguishable from -- the VMS boot banner
 * on what is supposed to read as a faithful VMS console.
 *
 * Lowered so only EMERG/ALERT/CRIT (bugcheck-class kernel faults) still
 * reach the console -- a real catastrophic kernel fault surfaces, exactly
 * as a VAX/Alpha would bugcheck to its own console, but routine INFO/WARN
 * module chatter does not. Does NOT touch the kernel's own log ring
 * buffer/log device (Linux /dev/kmsg, NetBSD /dev/klog) -- only the console
 * SINK -- so ovmx_boot_start_console_log_bridge()'s reader keeps seeing
 * every line for SYS$MANAGER:OPERATOR.LOG (vms-32a); kernel pr_info() call
 * sites are deliberately left untouched by this fix.
 *
 * Called once, from bare_metal_init(), immediately after
 * ovmx_boot_start_console_log_bridge() and before the first
 * ovmx_boot_load_module() call on either boot branch (flagless or
 * conversational) -- the earliest point in the boot path before any kernel
 * module is loaded. Idempotent: safe to call more than once. Best-effort: a
 * substrate that cannot lower its console level must never block or fail
 * boot -- this is console hygiene, not the fail-honest executive gate (that
 * is ovmx_boot_open_executive()).
 *
 * Linux: SYSLOG_ACTION_CONSOLE_LEVEL via syslog(2), falling back to writing
 * /proc/sys/kernel/printk's console_loglevel field. NetBSD: vms-f2e -- no
 * runtime-adjustable console print-level equivalent exists on that backend
 * yet; documented no-op there, never a faked success.
 */
void ovmx_boot_mute_kernel_console(void);

/*
 * ovmx_boot_load_module - load an OVMX executive/filesystem kernel module by
 * LOGICAL name ("vms" for the executive, "vmsfs" for the filesystem). Returns
 * 0 on success; -1 with errno set otherwise. The caller decides whether a
 * failure is survivable, and RELIES on errno being distinguishable:
 *   - ENOENT  the module image is not present (the executive treats this as
 *             the oracle's exact missing-system-file condition and halts).
 *   - EEXIST  the module is already loaded (survivable; the caller ignores it).
 * The backend sets errno to exactly what the underlying load reported, so
 * these tests behave identically to the raw syscall path.
 *
 * Linux: resolves the name to /lib/modules/<name>.ko and loads it with
 * finit_module(2) (open + SYS_finit_module + close). NetBSD: modctl(2)
 * MODCTL_LOAD by name (vms-f2e); its ENOENT/EEXIST map to the same meanings.
 */
int ovmx_boot_load_module(const char *name);

/*
 * ovmx_boot_open_executive - open the VMS executive device read/write with
 * close-on-exec, returning the descriptor or -1 with errno set. PID 1 holds
 * this open for the life of the system (it pins the executive module). The
 * device PATH is the substrate's, so it lives behind the seam.
 *
 * Linux: open("/dev/vms", O_RDWR | O_CLOEXEC). NetBSD: the same OVMX char
 * device by its NetBSD path (vms-f2e).
 */
int ovmx_boot_open_executive(void);

/*
 * ovmx_boot_system_disk_dev - the host device PATH of the system/boot disk
 * (DKA0:), for the operator-facing halt diagnostics. Returns a pointer to
 * storage valid for the life of the process. Linux: "/dev/vda". NetBSD: e.g.
 * "/dev/wd0" (vms-f2e).
 */
const char *ovmx_boot_system_disk_dev(void);

/*
 * ovmx_boot_system_disk_present - nonzero iff the system disk is present as a
 * mountable block device. This is the "finds it or does not boot" probe:
 * OVMX never installs one at boot, so absence is a fail-honest halt. Hides
 * both the device path and the substrate's notion of a mountable disk.
 * Linux: stat(system-disk) succeeds AND S_ISBLK.
 */
int ovmx_boot_system_disk_present(void);

/*
 * ovmx_boot_mount_system_disk - mount the system disk as the VMS filesystem
 * at `mountpoint` (the portable SYSDISK_MOUNT, chosen by ovmx_init.c). Returns
 * 0 on success, nonzero on failure -- a blank/unformatted volume that does not
 * mount as the VMS filesystem returns nonzero, and PID 1 halts (it does NOT
 * initialize or install). The device and filesystem type are the substrate's.
 * Linux: mount(system-disk, mountpoint, "vmsfs", 0, NULL).
 */
int ovmx_boot_mount_system_disk(const char *mountpoint);

/*
 * ovmx_boot_power_off - flush and power the machine off, PID 1's analogue of
 * the VAX halting to the console prompt after a fail-stop boot. Returns ONLY
 * if the substrate refused (no privilege to power off); the caller then
 * _exit()s. Linux: sync(); reboot(RB_POWER_OFF). NetBSD: sync();
 * reboot(RB_HALT | RB_POWERDOWN, NULL) (vms-f2e).
 */
void ovmx_boot_power_off(void);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_BOOT_H */
