/*
 * ovmx_boot_netbsd.c - the NetBSD realization of the OVMX boot-plumbing seam
 * (rd vms-f2e, epic vms-8e8; docs/design-p4-netbsd-vax-boot.md §4.5, GAP-C).
 *
 * DO NOT call these directly from ovmx_init.c through anything but
 * "ovmx_boot.h", which declares the op contract. See that header for what
 * each op means; this file is the NetBSD side, the peer of ovmx_boot_linux.c.
 * ONE ovmx_init.c compiles against EITHER backend with no `#ifdef __linux__`
 * fork of the boot logic (INV-DRIFT): the boot sequence and policy stay in
 * ovmx_init.c, only the substrate-specific syscall / device path / filesystem
 * type lives here.
 *
 * Every op maps to a PUBLIC, DOCUMENTED NetBSD kernel/libc interface, the
 * NetBSD twin of the exact Linux syscall the Linux backend makes:
 *
 *   ovmx_boot_kernel_filesystems_mounted -> /tmp is a tmpfs (statvfs(2))
 *   ovmx_boot_mount_kernel_filesystems   -> NetBSD mount(2): procfs on /proc,
 *                                           tmpfs on /tmp + /dev/shm,
 *                                           ptyfs on /dev/pts
 *   ovmx_boot_start_console_log_bridge   -> /dev/klog reader -> OPERATOR.LOG
 *   ovmx_boot_load_module                -> modctl(2) MODCTL_LOAD by name,
 *                                           with the in-kernel-executive
 *                                           fallback (design §5 risk 2)
 *   ovmx_boot_open_executive             -> open("/dev/vms", O_RDWR|O_CLOEXEC)
 *   ovmx_boot_system_disk_dev            -> "/dev/wd0"
 *   ovmx_boot_system_disk_present        -> stat("/dev/wd0") && S_ISBLK
 *   ovmx_boot_mount_system_disk          -> NetBSD mount(2) "vmsfs", fspec=dev
 *   ovmx_boot_power_off                  -> sync(); reboot(RB_HALT|RB_POWERDOWN)
 *
 * FAIL-HONEST (INV-6 / CLAUDE.md Rule 9). Exactly as on Linux, an op reports
 * the host's REAL result and NEVER fakes success. ovmx_boot_open_executive()
 * opens the REAL /dev/vms or returns -1; ovmx_boot_load_module() returns
 * modctl's real errno (or, in the in-kernel build, succeeds only on a live
 * /dev/vms node) and never no-ops to a bogus success.
 *
 * Clean-room (CLAUDE.md Rule 8): these call only public, documented NetBSD
 * interfaces (mount(2)/__mount50, modctl(2), reboot(2), statvfs(2), the
 * procfs/tmpfs/ptyfs mount-args, /dev/klog). No NetBSD or VSI/HPE source or
 * binary is copied.
 */

/*
 * Substrate-selected backend. This TU is the NetBSD realization; on every
 * other substrate the Linux backend serves and this file carries nothing
 * linkable. The typedef keeps the translation unit non-empty (ISO C forbids
 * an empty TU) so the host "compile every product .c" gates
 * (tests/integration/test_userspace_service_register.sh) see a clean object
 * with no symbols, and no include-path or flag dependency, on a non-NetBSD
 * host.
 */
#if !defined(__NetBSD__)

typedef int ovmx_boot_netbsd_backend_not_selected;

#else /* __NetBSD__ */

/* mount(2)/reboot(2)/sethostname(2)/statvfs f_fstypename live behind
 * _NETBSD_SOURCE; the build passes -D_NETBSD_SOURCE (build-ovmx-init-vax.sh),
 * not redefined here. */
#include "ovmx_boot.h"
#include "ovmx_layout.h"          /* VMS_OPERATOR_LOG */
#include "vmsfs/filespec.h"       /* vmsfs_to_linux_path for OPERATOR.LOG */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/module.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <fs/tmpfs/tmpfs_args.h>
#include <fs/ptyfs/ptyfs.h>
#include <miscfs/procfs/procfs.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The NetBSD host path for the OVMX ODS-2 system disk (DKA0:) -- the
 * substrate-specific name the seam exists to hide from ovmx_init.c (Linux:
 * "/dev/vda"). PINNED here by the bootable-disk assembly (vms-7b1), which is
 * exactly where the seam header said the partition/label would be nailed down.
 *
 * On NetBSD/vax under SIMH the OVMX ODS-2 volume is a SECOND MSCP disk (the
 * tests/lab-vax harness attaches the mastered volume on SIMH rq1 -> NetBSD
 * ra1), whole-disk partition `c' (the whole-disk partition on vax). The NetBSD
 * root fs is the FIRST MSCP disk (rq0 -> ra0), so DKA0: is ra1c -- the mirror
 * of the Linux runtime, where the NetBSD root is the initramfs and DKA0: is a
 * separate virtio disk (/dev/vda). On any other NetBSD substrate the disk-boot
 * assembly does not yet exist; keep the seam-header placeholder there.
 *
 * The executive device path "/dev/vms" is NOT hidden behind a macro: it is
 * written literally in ovmx_boot_open_executive() below, exactly as the Linux
 * backend does, so the Rule 9 backend gate can verify the backend really opens
 * the executive device and does not fake a descriptor. */
#if defined(__vax__)
#define OVMX_BOOT_SYSDISK_DEV   "/dev/ra1c"
#else
#define OVMX_BOOT_SYSDISK_DEV   "/dev/wd0"
#endif

/*
 * Wire PID 1's stdio to the console -- the ONE thing NetBSD's kernel does NOT
 * do for init that Linux's does. The Linux kernel opens /dev/console as fd
 * 0/1/2 for the init process (init/main.c), so ovmx_init.c's very first
 * printf() reaches the console with no help. The NetBSD kernel does NOT: it
 * leaves init's descriptors closed and expects init(8) to open /dev/console
 * itself. ovmx_init IS init on this substrate, so if it does not do the same,
 * EVERY line PID 1 prints -- the SYSKRNL banner, the executive-attach line,
 * the product banner, the mount narration, and the halt diagnostics -- goes to
 * a closed descriptor and never reaches the console (empirically: on the first
 * bring-up the modules loaded and the boot ran to its honest halt, but not one
 * character of PID 1 output appeared).
 *
 * This runs as an ELF constructor (ld.elf_so .init_array), BEFORE main(), so
 * even main()'s first printf lands on the console. It is the NetBSD-side
 * realization of a substrate fact -- exactly the kind of thing the boot seam
 * exists to absorb -- and needs no `#ifdef' in ovmx_init.c (INV-DRIFT): the
 * Linux backend has no such constructor because the Linux kernel already wired
 * the console. Guarded to PID 1 with a not-already-a-tty check, so running
 * STARTUP.EXE from a shell (e.g. the disk-assembly session) is untouched.
 * Best-effort: if /dev/console will not open, boot proceeds silently rather
 * than failing -- the fail-honest gate is the executive, not console I/O.
 */
__attribute__((constructor))
static void ovmx_netbsd_wire_console(void)
{
    struct stat st;
    int fd;

    if (getpid() != 1)
        return;                          /* only PID 1 owns the console */
    if (fstat(1, &st) == 0 && S_ISCHR(st.st_mode))
        return;                          /* stdout already a real console/tty */
    fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return;                          /* best-effort */
    (void)dup2(fd, 0);
    (void)dup2(fd, 1);
    (void)dup2(fd, 2);
    if (fd > 2)
        (void)close(fd);
}

int ovmx_boot_kernel_filesystems_mounted(void)
{
    /* NetBSD analogue of the Linux /proc/version probe (procfs mounted =>
     * plumbing up): ovmx_boot_mount_kernel_filesystems() puts a tmpfs on /tmp,
     * so /tmp reporting fstype "tmpfs" means this boot already ran (or a
     * normal multiuser userland is up). SAFE BIAS: anything else -- including
     * no statvfs answer -- reads as NOT up, so PID 1 sets the plumbing up
     * rather than skip it (skipping when it is really down would fail the
     * boot; re-running when it is really up is harmless best-effort). */
    struct statvfs vfs;
    if (statvfs("/tmp", &vfs) != 0)
        return 0;
    return strcmp(vfs.f_fstypename, "tmpfs") == 0;
}

int ovmx_boot_mount_kernel_filesystems(void)
{
    /* Best-effort, exactly like the Linux backend: individual mounts are not
     * error-checked (a missing pseudo-fs surfaces later as the concrete
     * failure that depends on it). NetBSD's mount(2) is
     * mount(type, dir, flags, data, data_len) -- a different shape from
     * Linux's mount(source, target, type, flags, data) -- and each filesystem
     * takes its own VERSIONED args struct (public, documented).
     *
     * The mount SET differs from Linux because the substrate differs, and each
     * difference is a real NetBSD fact, not a silent drop:
     *   - NO sysfs   : NetBSD has no /sys sysfs analogue.
     *   - NO devtmpfs: NetBSD /dev is a real directory (MAKEDEV), not a
     *                  kernel-populated devtmpfs -- there is nothing to mount
     *                  over it.
     *   - proc   -> NetBSD procfs (procfs(5))      on /proc
     *   - tmpfs  -> NetBSD tmpfs  (mount_tmpfs(8)) on /tmp and /dev/shm
     *   - devpts -> NetBSD ptyfs  (mount_ptyfs(8)) on /dev/pts
     */
    struct procfs_args pa;
    memset(&pa, 0, sizeof pa);
    pa.version = PROCFS_ARGSVERSION;
    mount("procfs", "/proc", 0, &pa, sizeof pa);

    struct tmpfs_args ta;
    memset(&ta, 0, sizeof ta);
    ta.ta_version  = TMPFS_ARGS_VERSION;
    ta.ta_root_uid = 0;
    ta.ta_root_gid = 0;
    ta.ta_root_mode = 01777;             /* sticky, world-writable -- /tmp */
    mount("tmpfs", "/tmp", 0, &ta, sizeof ta);

    mkdir("/dev/pts", 0755);
    struct ptyfs_args pta;
    memset(&pta, 0, sizeof pta);
    pta.version = PTYFS_ARGSVERSION;
    pta.gid  = 0;
    pta.mode = 0620;
    mount("ptyfs", "/dev/pts", 0, &pta, sizeof pta);

    mkdir("/dev/shm", 0755);
    struct tmpfs_args sa;
    memset(&sa, 0, sizeof sa);
    sa.ta_version  = TMPFS_ARGS_VERSION;
    sa.ta_root_uid = 0;
    sa.ta_root_gid = 0;
    sa.ta_root_mode = 01777;
    mount("tmpfs", "/dev/shm", 0, &sa, sizeof sa);

    return 0;
}

/* ---- kernel-log -> OPERATOR.LOG bridge (the NetBSD /dev/klog twin) -------- */

/*
 * The NetBSD twin of opcom_kmsg.c's /dev/kmsg reader (rd vms-32a). NetBSD's
 * kernel-log device is /dev/klog, which streams the kernel's printf output as
 * whole text lines (there is no per-record "<pri>,seq,...;text" framing to
 * parse the way Linux /dev/kmsg has). This routes every non-empty line to
 * SYS$MANAGER:OPERATOR.LOG only -- NEVER the console: the boot-console
 * facility+ident vocabulary is the boot orchestrator's, pinned against the
 * oracle by tests/qemu/test_boot_conformance.sh, and a kernel printk does not
 * belong on it (opcom_kmsg.h's top comment states this as a design invariant).
 * Lines wear the SYSKRNL facility, marking their origin as the host-kernel
 * layer underneath (same OVMX/SYSKRNL split opcom_kmsg.c uses). Best-effort:
 * if /dev/klog cannot be opened the thread exits quietly and boot is not
 * affected -- the fail-honest gate is the executive (/dev/vms), not this
 * operator-visibility aid.
 */
static void klog_append_operator_log(const char *line)
{
    char oplog_path[1024];
    FILE *f;

    if (vmsfs_to_linux_path(VMS_OPERATOR_LOG, oplog_path, sizeof oplog_path) == 1)
        f = fopen(oplog_path, "a");
    else
        f = NULL;
    if (!f)
        f = fopen("/tmp/OPERATOR.LOG", "a");   /* same fallback as sys$sndopr */
    if (!f)
        return;                                 /* best-effort */

    fputs(line, f);
    fclose(f);
}

static void *klog_thread_main(void *arg)
{
    int kfd;
    char raw[8192];

    (void)arg;

    kfd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (kfd < 0)
        return NULL;                            /* best-effort */

    for (;;) {
        struct pollfd pfd;
        int pr;
        ssize_t n;

        pfd.fd = kfd;
        pfd.events = POLLIN;
        pr = poll(&pfd, 1, 2000);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;                           /* periodic wake, keep following */

        n = read(kfd, raw, sizeof raw - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            continue;
        raw[n] = '\0';

        char *save = NULL;
        char *ln;
        for (ln = strtok_r(raw, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            char out[600];
            if (*ln == '\0')
                continue;
            snprintf(out, sizeof out, "%%SYSKRNL-I-KERNEL, %s\n", ln);
            klog_append_operator_log(out);
        }
    }

    close(kfd);
    return NULL;
}

void ovmx_boot_start_console_log_bridge(void)
{
    pthread_t tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* Best-effort: a bridge that fails to start must never block or fail boot
     * -- Rule 9's fail-honest gate is /dev/vms, not this visibility aid. */
    (void)pthread_create(&tid, &attr, klog_thread_main, NULL);
    pthread_attr_destroy(&attr);
}

/*
 * vms-300 (Linux/QEMU console-leak fix) / vms-f2e: NetBSD has no single
 * runtime-adjustable "console_loglevel" knob mirroring Linux's syslog(2)
 * SYSLOG_ACTION_CONSOLE_LEVEL -- kernel console verbosity there is governed
 * at boot time (boothowto flags), not by a live syscall this process can
 * issue. This backend's own console-noise question, if the NetBSD/vax boot
 * ever shows one, is tracked separately by the NetBSD boot design
 * (docs/design-p4-netbsd-vax-boot.md); this is a documented no-op, not a
 * faked success (INV-6 / CLAUDE.md Rule 9).
 */
void ovmx_boot_mute_kernel_console(void)
{
}

/* ---- executive module load: loadable module(9) OR compiled-in kernel ----- */

/*
 * OVMX_BOOT_EXEC selects how the executive reaches the kernel on NetBSD/vax
 * (design §5 risk 2 / rd vms-f78bb): the driver is EITHER a loadable
 * module(9), OR compiled into a custom NetBSD/vax kernel (the fallback for
 * when modules(9) on the thin VAX port cannot carry a cdevsw module). Both are
 * handled; the selection is runtime, and NEVER fakes success:
 *
 *   OVMX_BOOT_EXEC=module    always modctl(MODCTL_LOAD).
 *   OVMX_BOOT_EXEC=inkernel  never load; the executive is built into the
 *                            kernel -- require its /dev/vms node to be present
 *                            (fail-honest: a missing node is the oracle's
 *                            missing-system-file condition).
 *   OVMX_BOOT_EXEC=auto (default)  if /dev/vms is already a live char device
 *                            (in-kernel build, or an already-loaded module)
 *                            the executive really IS present -> succeed;
 *                            otherwise attempt modctl(MODCTL_LOAD) and report
 *                            its REAL errno.
 *
 * The errno contract ovmx_init.c relies on is preserved exactly, as on Linux:
 *   ENOENT  the module image is not present (the oracle's exact
 *           missing-system-file condition; the caller halts the boot).
 *   EEXIST  already loaded (survivable; the caller ignores it).
 */
enum exec_load_mode { EXEC_AUTO = 0, EXEC_MODULE = 1, EXEC_INKERNEL = 2 };

static enum exec_load_mode exec_load_mode(void)
{
    const char *m = getenv("OVMX_BOOT_EXEC");
    if (m && strcmp(m, "module") == 0)
        return EXEC_MODULE;
    if (m && strcmp(m, "inkernel") == 0)
        return EXEC_INKERNEL;
    return EXEC_AUTO;
}

static int dev_vms_present(void)
{
    struct stat st;
    return stat("/dev/vms", &st) == 0 && S_ISCHR(st.st_mode);
}

/*
 * exec_driver_registered - nonzero iff the `vms' cdevsw driver is REALLY
 * attached in the kernel right now (getdevmajor(3) returns its char major, not
 * NODEVMAJOR). This is the honest "is the executive live" test, distinct from
 * dev_vms_present() which only tells whether a /dev NODE FILE exists. They can
 * disagree on NetBSD in exactly the case the bootable disk creates: the disk
 * assembly PRE-CREATES /dev/vms (persistent, because there is no devfs to make
 * it on demand), so on a fresh boot the node file exists while nothing is
 * loaded yet. Opening such a node yields ENXIO. So the EXEC_AUTO short-circuits
 * -- "the executive is already here, no load needed" -- must key on the DRIVER
 * being registered, not on the stale node file existing, or a fresh boot would
 * skip the load and then fail to open a driverless node.
 */
static int exec_driver_registered(void)
{
    return getdevmajor("vms", S_IFCHR) != NODEVMAJOR;
}

/*
 * ensure_exec_node - make the executive's /dev/vms character node reachable
 * after the driver is loaded. This is the NetBSD analogue of what Linux
 * devtmpfs does AUTOMATICALLY the instant finit_module(2) attaches vms.ko:
 * NetBSD has no devfs, so a freshly-modctl-loaded cdevsw driver gets a
 * dynamically-assigned char major but no /dev entry, and something has to
 * create it. On Linux that "something" is the kernel; on NetBSD it is here,
 * so ovmx_boot_open_executive() finds a node to open.
 *
 * FAIL-HONEST (INV-6 / Rule 9), the whole reason this is not a blind mknod:
 * the major is looked up FROM THE KERNEL by driver name (getdevmajor(3),
 * libc) -- so the node can only ever name the REAL vms driver. If the driver
 * is not actually registered, getdevmajor returns NODEVMAJOR and NO node is
 * created; ovmx_boot_open_executive() then fails and PID 1 halts, exactly as
 * it must. It never fabricates a node pointing at a major that names no
 * executive.
 *
 * Idempotent and read-only-root-safe: if /dev/vms already exists (e.g. the
 * bootable-disk assembly pre-created it, or the driver was already loaded)
 * this is a no-op; if /dev is not writable the mknod simply fails and the
 * pre-existing node is used. Only the executive ("vms") gets a node -- vmsfs
 * is a VFS with no device node.
 */
static void ensure_exec_node(void)
{
    devmajor_t cmaj;

    if (dev_vms_present())
        return;                          /* already there (pre-created / loaded) */
    cmaj = getdevmajor("vms", S_IFCHR);  /* ask the kernel for the REAL major */
    if (cmaj == NODEVMAJOR)
        return;                          /* driver not registered -> no node */
    (void)mknod("/dev/vms", S_IFCHR | 0666, makedev(cmaj, 0));
}

int ovmx_boot_load_module(const char *name)
{
    enum exec_load_mode mode = exec_load_mode();
    int is_exec = (strcmp(name, "vms") == 0);
    modctl_load_t ml;
    int saved;

    if (mode == EXEC_INKERNEL) {
        /* Nothing to load -- the driver is compiled into the custom kernel.
         * For the executive, REQUIRE its device node (fail-honest, INV-6); for
         * vmsfs (a VFS, no device node) it is present by construction. */
        if (is_exec && !dev_vms_present()) {
            errno = ENOENT;
            return -1;
        }
        return 0;
    }

    /* AUTO: if the executive device is already live, it is really here
     * (in-kernel, or an already-loaded module) -- no load needed. */
    if (mode == EXEC_AUTO && is_exec && exec_driver_registered())
        return 0;

    /* module path: modctl(MODCTL_LOAD) by name. The kernel resolves a bare
     * module name through its module_path; ml_flags/ml_props are unused. */
    memset(&ml, 0, sizeof ml);
    ml.ml_filename = name;                       /* "vms" / "vmsfs" */
    if (modctl(MODCTL_LOAD, &ml) != 0) {
        saved = errno;                           /* stat() below clobbers errno */
        /* AUTO: if modules(9) cannot load it but /dev/vms is nonetheless live
         * (a thin-modules VAX kernel that built the driver in), the executive
         * really IS present -- succeed. This never fakes: it succeeds only on
         * a real live /dev/vms char device. */
        if (mode == EXEC_AUTO && is_exec && exec_driver_registered())
            return 0;
        errno = saved;                           /* preserve modctl's errno */
        return -1;
    }
    /* The executive driver is now loaded: give it its /dev/vms node the way
     * Linux devtmpfs would have, so ovmx_boot_open_executive() can open it.
     * A VFS module (vmsfs) has no device node -- ensure_exec_node() only acts
     * for the executive and is a no-op otherwise. */
    if (is_exec)
        ensure_exec_node();
    return 0;
}

int ovmx_boot_open_executive(void)
{
    return open("/dev/vms", O_RDWR | O_CLOEXEC);
}

const char *ovmx_boot_system_disk_dev(void)
{
    return OVMX_BOOT_SYSDISK_DEV;
}

int ovmx_boot_system_disk_present(void)
{
    struct stat st;
    return stat(OVMX_BOOT_SYSDISK_DEV, &st) == 0 && S_ISBLK(st.st_mode);
}

int ovmx_boot_mount_system_disk(const char *mountpoint)
{
    /* NetBSD mount(2) carries the device INSIDE the fs args (fspec), unlike
     * Linux's mount(dev, mp, type, ...). Every NetBSD disk filesystem's args
     * struct begins with `char *fspec` (ffs / lfs / ext2fs / msdosfs / cd9660
     * / ...); the OVMX ODS-2 vnode backend (rd vms-308 / vms-544d) uses exactly
     * this shape -- `struct vmsfs_args { char *fspec; }`
     * (src/kernel-netbsd/vmsfs/vmsfs_nb.h) -- so this modelled struct is the
     * real args blob VFS_MOUNT consumes, byte-for-byte, not a stub.
     *
     * READ-WRITE (rd vms-e7a): the OVMX ODS-2 vnode backend now registers real
     * write VOPs (VOP_SETATTR/WRITE/CREATE/MKDIR/REMOVE, alongside the
     * existing VOP_LOOKUP/READ/READDIR/the exec-from-vmsfs pager), so the
     * system volume mounts read-write here -- matching real VMS, which mounts
     * its system disk read-write, and matching the Linux backend's mount mode.
     * This is what lets PROVISION.EXE stamp UIC file ownership
     * (provision_ownership()) and STARTUP write SYSUAF logs / account-dir
     * files onto the mounted volume. tests/lab-vax/run-vmsfs.sh's read-only
     * mount+read proof still passes MNT_RDONLY explicitly (a caller that asks
     * for read-only still gets an honestly read-only mount: no bitmap load,
     * every write VOP refuses with EROFS). A blank or unformatted volume still
     * fails to mount (nonzero return), and PID 1 halts (it does NOT initialize
     * or install -- design-init-scope.md §1). */
    struct vmsfs_args { char *fspec; } args;
    args.fspec = (char *)OVMX_BOOT_SYSDISK_DEV;
    return mount("vmsfs", mountpoint, 0, &args, sizeof args);
}

void ovmx_boot_power_off(void)
{
    sync();
    reboot(RB_HALT | RB_POWERDOWN, NULL);
    /* Only reached without privilege to power off; the caller then _exit()s. */
}

#endif /* __NetBSD__ */
