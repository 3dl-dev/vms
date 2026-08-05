/*
 * ovmx_init.c - OVMX Boot Orchestrator (STARTUP.EXE)
 *
 * Unified PID 1 / ENTRYPOINT for OVMX on both Docker and bare-metal (QEMU).
 * Handles the entire boot sequence: filesystem setup, kernel module loading,
 * VMS directory provisioning, SYSUAF user provisioning, daemon startup,
 * boot banner, and console login loop.
 *
 * No shell scripts, no busybox, no /etc/passwd — SYSUAF is the user database.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <errno.h>

#include "vms/pcb.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "ovmx_layout.h"
#include "ovmx_identity.h"
#include "ssdef.h"
/* PID 1's identity is established THROUGH the executive, not declared. */
#include "vms_kif.h"

#define SYSDISK_DEV      "/dev/vda"
#define INITRAMFS_BACKUP "/tmp/initramfs_vms"

/*
 * Binary/library search paths — initialized at runtime after device
 * table is populated so VMS specs can be translated to Linux paths.
 * Docker puts binaries in standard paths, QEMU in SYS$SYSTEM.
 */
static char sysexe_linux[512];
static char syslib_linux[512];
static const char *bin_search_dirs[5];
static const char *lib_search_dirs[4];

static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static volatile sig_atomic_t shutdown_requested = 0;
static int blkdev_mode = 0;

/*
 * Translate a VMS filespec to a Linux path.
 * Wrapper that returns a static buffer — use immediately or copy.
 * Falls back to SYSDISK_MOUNT-relative path if translation fails.
 */
static const char *vms_to_linux(const char *vms_spec, char *buf, size_t bufsz)
{
    if (vmsfs_to_linux_path(vms_spec, buf, bufsz) == 1)
        return buf;
    /* Fallback: shouldn't happen after device table init */
    snprintf(buf, bufsz, "%s", vms_spec);
    return buf;
}

/*
 * Initialize runtime search paths after device table + LNM are live.
 */
static void init_search_paths(void)
{
    vms_to_linux(VMS_SYSEXE, sysexe_linux, sizeof(sysexe_linux));
    vms_to_linux(VMS_SYSLIB, syslib_linux, sizeof(syslib_linux));

    bin_search_dirs[0] = sysexe_linux;
    bin_search_dirs[1] = VMS_SYSTEM_DIR;
    bin_search_dirs[2] = "/bin";
    bin_search_dirs[3] = "/sbin";
    bin_search_dirs[4] = NULL;

    lib_search_dirs[0] = syslib_linux;
    lib_search_dirs[1] = VMS_LIBRARY_DIR;
    lib_search_dirs[2] = "/lib";
    lib_search_dirs[3] = NULL;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    shutdown_requested = 1;
}

static void sigchld_handler(int sig)
{
    (void)sig;
    /* Reap all zombies */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

/* ------------------------------------------------------------------ */
/* Bare-metal bootstrap                                               */
/* ------------------------------------------------------------------ */

/*
 * A required executive image would not load. Report it the way VMS does and
 * halt -- the system does not come up.
 *
 * PINNED TO THE ORACLE, NOT INVENTED. Observed on the reference lab's
 * OpenVMS VAX V7.3 (~/vax/cluster, node VAX2) on 2026-07-29 by renaming
 * SYS$COMMON:[SYS$LDR]EXCEPTION.EXE aside and cold-booting `B/R5:10000000
 * DUA0`. Console capture archived at
 * ~/vax/cluster/captures/vax2-execinit-missing-exception-2026-07-29.log:
 *
 *     %EXECINIT, error loading system file - EXCEPTION.EXE R0 = 00000910
 *     ?06 HLT INST
 *             PC = 871306A6
 *     >>>
 *
 * Four properties of that message are deliberate here, because they are the
 * authenticity tells:
 *   - the facility is EXECINIT, not SYSBOOT (an earlier wave invented
 *     "%SYSBOOT-F-LDFAIL"; the complete VAX 7.3 SYSBOOT message set was
 *     dumped via HELP/MESSAGE/FACILITY=SYSBOOT and contains no such thing);
 *   - there is NO severity letter and NO mnemonic -- it is a bare
 *     "%EXECINIT," message, unlike almost every other VMS message;
 *   - the image is named as a BARE FILENAME, not a full filespec;
 *   - the status is printed raw as "R0 = " + 8 hex digits.
 * The machine then halts to the console prompt. It does not continue, does
 * not bugcheck, and produces no crash dump. So OVMX's fail-stop boot is not
 * an OVMX invention after all -- it is what VMS actually does.
 *
 * As PID 1 we power the machine off rather than exit: an exiting PID 1
 * panics the kernel and leaves QEMU wedged, whereas a clean power-off ends
 * the boot with the diagnostic still on the console -- OVMX's analogue of
 * the VAX halting to >>>. If reboot(2) is unavailable (no CAP_SYS_BOOT,
 * e.g. the dead-legacy container), exit nonzero instead.
 */

/*
 * SS$_NOSUCHFILE as the ORACLE reports it: R0 held 00000910 in the capture
 * above, and F$MESSAGE(%X00000910) on the same system decodes to
 * "%SYSTEM-W-NOSUCHFILE, no such file".
 *
 * DELIBERATELY NOT TAKEN FROM ssdef.h, which defines SS$_NOSUCHFILE as 2696
 * (0xA88) and disagrees with the oracle. That drift is real and already
 * tracked (vms-556 / vms-c90, alongside SS$_NOSUCHDEV 2680 vs the oracle's
 * 2312) and needs OPERATOR SIGN-OFF -- a VMS constant is never
 * self-certified, so this file does not "fix" ssdef.h in passing. The value
 * below is used only to reproduce an observed console line, and is pinned to
 * the observation that produced it.
 */
#define OVMX_R0_NOSUCHFILE_ORACLE  0x00000910u

static void halt_now(void)
{
    fflush(NULL);

    if (getpid() == 1) {
        sync();
        reboot(RB_POWER_OFF);
        /* Only reached without CAP_SYS_BOOT. */
    }
    _exit(1);
}

/*
 * The oracle-pinned path: reproduces the VAX 7.3 capture byte-exact
 * ("%EXECINIT, error loading system file - <FILE> R0 = <status>"). Call
 * this ONLY when the failure being reported is the exact oracle condition
 * (a required executive image is missing, ENOENT) -- never for a condition
 * VMS is never in (see ovmx_exec_halt below, and Rule 10).
 */
static void execinit_halt(const char *image, const char *detail, unsigned int r0)
{
    fprintf(stderr, "%%EXECINIT, error loading system file - %s R0 = %08X\n",
            image, r0);

    /*
     * OVMX DESIGN CHOICE (Rule 8), labelled as such: VMS prints nothing more
     * -- the "?06 HLT INST" line comes from the VAX console firmware, not
     * from VMS. OVMX has no such firmware layer, and the underlying Linux
     * error carries information a VMS status cannot, so it is emitted as an
     * explicitly OVMX-facility line rather than dressed up as VMS output.
     */
    if (detail)
        fprintf(stderr, "%%OVMX-I-EXECINIT, %s\n", detail);
    halt_now();
}

/*
 * The NOT-oracle-pinned path: a fatal executive-attach failure for which VMS
 * has no analogue at all -- either a module-load errno other than the
 * oracle's ENOENT, or /dev/vms (a Linux device node; VMS has no such thing
 * to open) refusing to open. Rule 10 is explicit that a plausible-looking
 * VMS message may never be invented for a condition VMS never faces; the
 * bare "%EXECINIT, error loading system file - <FILE>" shape (no R0) that
 * used to come out of this function's other branch was exactly that
 * invention. This path wears the OVMX facility instead of EXECINIT.
 */
static void ovmx_exec_halt(const char *what, const char *detail)
{
    fprintf(stderr, "%%OVMX-F-EXECINIT, %s\n", what);
    if (detail)
        fprintf(stderr, "%%OVMX-I-EXECINIT, %s\n", detail);
    halt_now();
}

/*
 * Load a kernel module. Returns 0 on success, -1 with errno set otherwise.
 * Callers decide whether a failure is survivable -- for the executive it is
 * not (see executive_attach).
 */
static int load_kernel_module(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long rc = syscall(SYS_finit_module, fd, "", 0);
    int saved = errno;
    close(fd);
    if (rc != 0) {
        errno = saved;
        return -1;
    }
    return 0;
}

/*
 * Bind this system to its executive, permanently.
 *
 * The executive is INTEGRAL, not optional. On OpenVMS the executive IS the
 * operating system: it is resident before the first process exists, and no
 * image can run without it. OVMX therefore does not have -- and must never
 * grow -- a mode in which it comes up without one. There is no graceful
 * degradation to design here, because the degraded state is one VMS can
 * never be in; a handled-but-impossible-on-VMS state is itself an
 * authenticity defect (CLAUDE.md Rule 9).
 *
 * Two guarantees are established here, and everything downstream depends on
 * both:
 *
 *   1. BOOT IS FATAL. If vms.ko will not load, or /dev/vms will not open,
 *      the system does not come up. After this function returns, every image
 *      OVMX will ever run is guaranteed a reachable executive -- which is
 *      what lets the system services drop their absent-executive paths
 *      entirely rather than reporting a status no caller can observe.
 *
 *   2. THE EXECUTIVE IS PINNED FOR THE LIFE OF THE SYSTEM. PID 1 holds this
 *      descriptor open and never closes it. vms.ko's file_operations carry
 *      .owner = THIS_MODULE (src/kernel/vms_module.c), so an open descriptor
 *      holds a module reference: `rmmod vms` fails with EBUSY for as long as
 *      OVMX is running. Mid-life loss of the executive is PREVENTED, not
 *      handled -- there is no response to design because the event cannot
 *      occur.
 *
 * Boundary of guarantee 2, stated honestly: a privileged operator on the
 * host can still unlink the /dev/vms node itself. That does not unload or
 * disturb the running executive (this descriptor stays valid), but it would
 * stop new processes from opening it by path. OVMX does not defend against
 * a privileged actor deliberately sabotaging the running system, exactly as
 * VMS does not defend against one corrupting a resident executive image.
 */
static int executive_fd = -1;

static void executive_attach(void)
{
    if (executive_fd >= 0)
        return;                 /* already attached; idempotent by design */

    if (load_kernel_module("/lib/modules/vms.ko") != 0 && errno != EEXIST) {
        if (errno == ENOENT) {
            /* The oracle's exact condition -- a required executive image
             * that is not there -- so its exact status is reproduced. */
            execinit_halt("VMS.KO", strerror(errno), OVMX_R0_NOSUCHFILE_ORACLE);
        } else {
            /* Other load errnos have no oracle-pinned VMS status, and one is
             * not invented for them (see OVMX_R0_NOSUCHFILE_ORACLE). */
            ovmx_exec_halt("error loading system file - VMS.KO", strerror(errno));
        }
    }

    executive_fd = open("/dev/vms", O_RDWR | O_CLOEXEC);
    if (executive_fd < 0) {
        /* No oracle analogue: a VMS executive has no device node to open, so
         * VMS is never in this state and prints no status for it. This is an
         * OVMX event, not a VMS one -- it must not wear the EXECINIT
         * facility (Rule 10). */
        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));
    }
    printf("%%OVMX-I-EXEC, VMS executive attached on /dev/vms\n");
}

/*
 * Recursive copy of a directory tree (for vmsfs backing dir).
 * Handles regular files, directories, and symlinks.
 */
static void copy_recursive(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0)
        return;

    mkdir(dst, st.st_mode & 07777);

    DIR *d = opendir(src);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        char src_path[512], dst_path[512];
        int src_len = snprintf(src_path, sizeof(src_path), "%s/%s", src, ent->d_name);
        int dst_len = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, ent->d_name);
        if (src_len >= (int)sizeof(src_path) || dst_len >= (int)sizeof(dst_path)) {
            fprintf(stderr, "%%STARTUP-W-PATHTRUNC, path too long, skipping: %s/%s\n",
                    src, ent->d_name);
            continue;
        }

        struct stat es;
        if (lstat(src_path, &es) != 0)
            continue;

        if (S_ISDIR(es.st_mode)) {
            copy_recursive(src_path, dst_path);
        } else if (S_ISLNK(es.st_mode)) {
            char link_target[512];
            ssize_t len = readlink(src_path, link_target, sizeof(link_target) - 1);
            if (len > 0) {
                link_target[len] = '\0';
                symlink(link_target, dst_path);
            }
        } else if (S_ISREG(es.st_mode)) {
            int sfd = open(src_path, O_RDONLY);
            if (sfd < 0) continue;
            int dfd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, es.st_mode & 07777);
            if (dfd < 0) { close(sfd); continue; }
            char buf[4096];
            ssize_t n;
            while ((n = read(sfd, buf, sizeof(buf))) > 0) {
                ssize_t total = 0;
                while (total < n) {
                    ssize_t w = write(dfd, buf + total, (size_t)(n - total));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        fprintf(stderr, "%%STARTUP-W-COPYERR, write failed for %s: %s\n",
                                dst_path, strerror(errno));
                        goto copy_done;
                    }
                    total += w;
                }
            }
            copy_done:
            close(dfd);
            close(sfd);
        }
    }
    closedir(d);
}

/*
 * Bare-metal bootstrap: mount filesystems, set hostname, load kernel
 * modules, mount vmsfs. Called when running as PID 1 on bare metal
 * or QEMU — not inside a Docker container.
 *
 * Two modes:
 *   1. Block-device mode: /dev/vda (virtio disk) present — mount vmsfs
 *      directly on the block device. If blank, run INITIALIZE.EXE first.
 *   2. Overlay mode: no block device — copy /vms to backing dir, mount
 *      vmsfs overlay (ephemeral, existing behavior).
 */
static void bare_metal_init(void)
{
    /* Mount essential filesystems */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mkdir("/dev/shm", 0755);
    mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);

    /* Set hostname */
    sethostname("OVMX", 4);

    /* The executive comes up before anything else runs -- including
     * INITIALIZE.EXE below, which is an OVMX image like any other and so is
     * covered by the same guarantee. */
    executive_attach();

    /* vmsfs.ko is the filesystem, not the executive; a failure here shows up
     * as a mount failure below, which already has its own handling. The
     * executive itself is loaded and pinned by executive_attach(), called
     * from main() once /dev exists. */
    if (load_kernel_module("/lib/modules/vmsfs.ko") != 0 && errno != EEXIST) {
        fprintf(stderr, "%%STARTUP-W-MODFAIL, failed to load vmsfs.ko: %s\n",
                strerror(errno));
    }

    struct stat vms_st;
    if (stat(SYSDISK_MOUNT, &vms_st) != 0)
        return;  /* No system disk root in initramfs */

    /* Check for system disk (virtio block device) */
    struct stat vda_st;
    if (stat(SYSDISK_DEV, &vda_st) == 0 && S_ISBLK(vda_st.st_mode)) {
        printf("%%STARTUP-I-SYSDISK, mounting system disk DKA0:\n");

        /* Back up initramfs before mounting over it */
        copy_recursive(SYSDISK_MOUNT, INITRAMFS_BACKUP);

        /* Try mounting — succeeds if disk is already formatted */
        int rc = mount(SYSDISK_DEV, SYSDISK_MOUNT, "vmsfs", 0, NULL);
        if (rc != 0) {
            /* Disk is blank or unformatted — run INITIALIZE.EXE */
            printf("%%STARTUP-I-INIT, initializing blank system disk\n");

            char init_path[256];
            snprintf(init_path, sizeof(init_path),
                     "%s/SYS0/SYSCOMMON/SYSEXE/INITIALIZE.EXE",
                     INITRAMFS_BACKUP);

            pid_t pid = fork();
            if (pid == 0) {
                execl(init_path, "INITIALIZE.EXE",
                      SYSDISK_DEV, "OVMX", (char *)NULL);
                _exit(1);
            }
            if (pid > 0) {
                int ws;
                waitpid(pid, &ws, 0);
            }

            rc = mount(SYSDISK_DEV, SYSDISK_MOUNT, "vmsfs", 0, NULL);
        }

        if (rc == 0) {
            printf("%%STARTUP-I-MOUNTED, system disk DKA0: mounted\n");
            blkdev_mode = 1;
            return;
        }

        printf("%%STARTUP-W-MOUNTFAIL, system disk mount failed (errno=%d), using overlay\n",
               errno);
        /* Fall through to overlay mode */
    }

    /* Overlay mode — no system disk or block-device mount failed */
    mkdir("/var", 0755);
    mkdir("/var/vmsfs", 0755);

    /* Use backup if we already copied for a failed blkdev attempt */
    struct stat backup_st;
    if (stat(INITRAMFS_BACKUP, &backup_st) == 0)
        copy_recursive(INITRAMFS_BACKUP, "/var/vmsfs");
    else
        copy_recursive(SYSDISK_MOUNT, "/var/vmsfs");

    mount("none", SYSDISK_MOUNT, "vmsfs", 0, "backing=/var/vmsfs,case_blind=1");
}

/* ------------------------------------------------------------------ */
/* System install / boot separation                                   */
/* ------------------------------------------------------------------ */

/*
 * Copy a single file from src to dst if dst does not exist.
 * Used to seed system data files (SYSUAF.DAT, RIGHTSLIST.DAT,
 * STARTUP.COM) from initramfs to the mounted system disk on first boot.
 */
static void copy_seed_file(const char *src, const char *dst, mode_t mode)
{
    int sfd = open(src, O_RDONLY);
    if (sfd < 0)
        return;  /* No seed file in initramfs */

    int dfd = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (dfd < 0) {
        close(sfd);
        return;  /* Already exists or cannot create */
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t written = write(dfd, buf, (size_t)n);
        if (written < 0)
            break;
    }

    close(dfd);
    close(sfd);
}

/*
 * Provision seed data files onto the system disk.
 * On first boot (or after disk re-initialization), these files may
 * not exist on the system disk. Copy seed versions from the initramfs
 * backup so that SYSUAF, RIGHTSLIST, and STARTUP.COM are available
 * from the mounted system disk, not the initramfs.
 *
 * On subsequent boots, existing files are preserved (no overwrite).
 */
static void provision_seed_files(void)
{
    char src[512], dst[512];

    /* SYSUAF.DAT → SYS$SYSTEM (0600: contains password hashes) */
    vms_to_linux(VMS_SYSUAF_PATH, dst, sizeof(dst));
    snprintf(src, sizeof(src), "%s/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT",
             INITRAMFS_BACKUP);
    copy_seed_file(src, dst, 0600);

    /* RIGHTSLIST.DAT → SYS$SYSTEM (0600: security data) */
    vms_to_linux(VMS_RIGHTSLIST_PATH, dst, sizeof(dst));
    snprintf(src, sizeof(src), "%s/SYS0/SYSCOMMON/SYSEXE/RIGHTSLIST.DAT",
             INITRAMFS_BACKUP);
    copy_seed_file(src, dst, 0600);

    /* STARTUP.COM → SYS$MANAGER (0644: command procedure) */
    vms_to_linux(VMS_STARTUP_PATH, dst, sizeof(dst));
    snprintf(src, sizeof(src), "%s/SYS0/SYSCOMMON/SYSMGR/STARTUP.COM",
             INITRAMFS_BACKUP);
    copy_seed_file(src, dst, 0644);

    /* SYSTARTUP_VMS.COM → SYS$MANAGER (0644: command procedure).
     * STARTUP.COM invokes it unconditionally, so a system disk installed
     * before this file existed must gain it here or every boot after an
     * upgrade would report it missing. */
    vms_to_linux(VMS_SYSTARTUP_PATH, dst, sizeof(dst));
    snprintf(src, sizeof(src), "%s/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM",
             INITRAMFS_BACKUP);
    copy_seed_file(src, dst, 0644);
}

/*
 * Check if the system is already installed on the system disk.
 * DCL.EXE in SYSEXE is the marker — if it exists (file or symlink),
 * a prior install populated the tree and we can skip straight to boot.
 */
static int is_system_installed(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/DCL.EXE", sysexe_linux);
    struct stat st;
    return (lstat(path, &st) == 0);
}

/*
 * Create the VMS directory tree following ODS-2 conventions.
 * mkdir is idempotent — safe to call even if directories already exist.
 *
 * MFD [000000]:         /vms/
 * [SYS0]                /vms/SYS0/
 * [SYS0.SYSCOMMON]      /vms/SYS0/SYSCOMMON/
 * SYS$SYSTEM [SYSEXE]   /vms/SYS0/SYSCOMMON/SYSEXE/
 * SYS$LIBRARY [SYSLIB]  /vms/SYS0/SYSCOMMON/SYSLIB/
 * SYS$MANAGER [SYSMGR]  /vms/SYS0/SYSCOMMON/SYSMGR/
 * SYS$HELP [SYSHLP]     /vms/SYS0/SYSCOMMON/SYSHLP/
 * [USERS]               /vms/USERS/
 */
static void provision_dirs(void)
{
    char path[512];
    int n;

    /* MFD and intermediate directories */
    mkdir(SYSDISK_MOUNT, 0755);
    vms_to_linux(VMS_SYSROOT, path, sizeof(path));
    /* Validate path wasn't truncated before manipulating it */
    n = (int)strlen(path);
    if (n <= 0 || n >= (int)sizeof(path) - 1) {
        fprintf(stderr, "%%STARTUP-E-PATHTRUNC, VMS_SYSROOT path too long\n");
        return;
    }
    /* Create SYS0 first, then SYS0/SYSCOMMON */
    char *last_slash = strrchr(path, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(path, 0755);   /* SYS0 */
        *last_slash = '/';
    }
    mkdir(path, 0755);       /* SYS0/SYSCOMMON */

    /* System directories */
    mkdir(sysexe_linux, 0755);
    mkdir(syslib_linux, 0755);
    vms_to_linux(VMS_SYSMGR, path, sizeof(path));
    mkdir(path, 0755);
    vms_to_linux(VMS_SYSHLP, path, sizeof(path));
    mkdir(path, 0755);
    vms_to_linux(VMS_USERS, path, sizeof(path));
    mkdir(path, 0755);
    vms_to_linux(VMS_SYSTMP, path, sizeof(path));
    mkdir(path, 0755);

    mkdir("/tmp/ovmx", 0755);
    mkdir("/tmp/ovmx/locks", 0755);
}

/*
 * Create a symlink at vms_path → discovered binary location.
 * Searches bin_search_dirs for the named file. Skips if vms_path
 * already exists (binary placed directly in SYS$SYSTEM by initramfs).
 */
static void ensure_vms_binary(const char *vms_path, const char *name)
{
    struct stat st;

    for (int i = 0; bin_search_dirs[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", bin_search_dirs[i], name);
        if (stat(path, &st) == 0) {
            /* Atomic: just try symlink(), handle EEXIST */
            if (symlink(path, vms_path) == 0 || errno == EEXIST)
                return;
        }
    }
}

/*
 * Create a symlink at vms_path → discovered library location.
 */
static void ensure_vms_library(const char *vms_path, const char *name)
{
    struct stat st;

    for (int i = 0; lib_search_dirs[i]; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", lib_search_dirs[i], name);
        if (stat(path, &st) == 0) {
            /* Atomic: just try symlink(), handle EEXIST */
            if (symlink(path, vms_path) == 0 || errno == EEXIST)
                return;
        }
    }
}

/*
 * Populate SYS$SYSTEM and SYS$LIBRARY with VMS-named binaries/images.
 * On QEMU, files are already at VMS paths (initramfs). On Docker,
 * creates symlinks from SYSEXE/SYSLIB to /usr/local/{bin,lib}/.
 */
static void provision_symlinks(void)
{
    char exe_path[512], lib_path[512];

    /* [SYS0.SYSCOMMON.SYSEXE] executables */
    const char *exes[] = {
        "LOGINOUT.EXE", "DCL.EXE", "HELP.EXE", "AUTHORIZE.EXE",
        "MAIL.EXE", "MONITOR.EXE", "VMSSSHD.EXE",
        "STARTUP.EXE", NULL
    };
    for (int i = 0; exes[i]; i++) {
        snprintf(exe_path, sizeof(exe_path), "%s/%s", sysexe_linux, exes[i]);
        ensure_vms_binary(exe_path, exes[i]);
    }

    /* [SYS0.SYSCOMMON.SYSLIB] shareable images */
    const char *libs[] = {
        "LIBVMS$SHR.EXE", "LIBVMSPROCESS$SHR.EXE", "LIBVMSLNM$SHR.EXE",
        "LIBVMSFS$SHR.EXE", "LIBVMSRMS$SHR.EXE", NULL
    };
    for (int i = 0; libs[i]; i++) {
        snprintf(lib_path, sizeof(lib_path), "%s/%s", syslib_linux, libs[i]);
        ensure_vms_library(lib_path, libs[i]);
    }
}

/*
 * Split one SYSUAF record into the fields this file cares about.
 *
 * SYSUAF format (pipe-delimited, 0-indexed):
 *   0 USERNAME | 1 HASH | 2 UIC_GROUP | 3 UIC_MEMBER
 *   4 DEFAULT_DIR | 5 FLAGS | 6 PRIVILEGES
 *
 * `line` is modified in place. Returns 0 if the record parsed, -1 if it
 * is a comment, blank, or malformed.
 */
static int sysuaf_split(char *line, char **user, uint32_t *grp, uint32_t *mem,
                        char **defdir)
{
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
        return -1;

    char *f[7];
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < 7; p++) {
        if (*p == '|') { *p = '\0'; f[n++] = p + 1; }
    }
    if (n < 5)
        return -1;
    for (char *p = f[n - 1]; *p; p++) {
        if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    }

    *user   = f[0];
    /*
     * OCTAL (vms-e60), and this site is the reason the item's own enumeration
     * was not enough. SYSUAF.DAT's UIC fields are octal; derivation in
     * src/libvms/rtl/sysuaf.c.
     *
     * MEASURED, by the UAT rather than by review: fixing the other ten sites
     * and leaving this one decimal made every non-SYSTEM login unable to write
     * its OWN login directory. This function feeds own_tree(), which chowns
     * SYS$SYSDEVICE:[USERS.<name>] to the account's UIC, while LOGINOUT
     * setuid()s to the UIC it reads through libvms. With the two on different
     * bases GUEST's directory was owned by 201:200 and GUEST ran as 129:128,
     * so `COPY SYS$MANAGER:LOGIN.COM UATUSER.TXT` returned %RMS-E-CRE.
     *
     * That is exactly the failure mode a partial base change produces: it does
     * not restore the old behaviour, it relocates the disagreement to a new
     * pair of subsystems. Any future change to this base must move all of the
     * sites listed in sysuaf.c together.
     */
    *grp    = (uint32_t)strtoul(f[2], NULL, 8);
    *mem    = (uint32_t)strtoul(f[3], NULL, 8);
    *defdir = f[4];
    return 0;
}

/*
 * Give one filesystem object to a UIC, without following symlinks.
 *
 * lchown() and not chown(): in overlay mode SYS$SYSTEM holds symlinks to
 * /usr/local/bin, and re-owning a symlink must not re-own the Linux
 * binary it points at.
 */
static void own_object(const char *path, uint32_t uic_group, uint32_t uic_member)
{
    if (lchown(path, (uid_t)uic_member, (gid_t)uic_group) != 0 &&
        errno != ENOENT)
        fprintf(stderr, "%%STARTUP-W-OWNER, cannot set owner of %s: %s\n",
                path, strerror(errno));
}

/*
 * Give a directory and everything beneath it to a UIC.
 */
static void own_tree(const char *path, uint32_t uic_group, uint32_t uic_member)
{
    own_object(path, uic_group, uic_member);

    DIR *d = opendir(path);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        char child[512];
        int len = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (len < 0 || len >= (int)sizeof(child))
            continue;

        struct stat es;
        if (lstat(child, &es) != 0)
            continue;

        if (S_ISDIR(es.st_mode))
            own_tree(child, uic_group, uic_member);
        else
            own_object(child, uic_group, uic_member);
    }
    closedir(d);
}

/*
 * Read SYSUAF and create home directories for each user, OWNED BY THAT
 * USER'S UIC.
 *
 * WHY THE OWNERSHIP IS PART OF PROVISIONING AND NOT PART OF LOGIN
 * (vms-2b8 round 7). Until LOGINOUT started dropping to the
 * authenticated user's credentials, every VMS session on OVMX ran as
 * Linux root, so it did not matter who owned anything: root passes
 * every DAC check. The moment a session really becomes UIC [1,4], a
 * tree installed as root:root is a tree that VMS's own SYSTEM account
 * cannot write -- and that is not a VMS state.
 *
 * On OpenVMS the account's directory is created BY THE ACCOUNT
 * PROVISIONING (the System Manager's Manual add-user procedure is
 * AUTHORIZE ADD followed by CREATE/DIRECTORY .../OWNER=[g,m]); LOGINOUT
 * does not create it and does not re-own it. OVMX does the same here,
 * in PID 1, before any other process exists.
 *
 * SYSUAF format: USERNAME|HASH|UIC_GROUP|UIC_MEMBER|DEFAULT_DIR|FLAGS|PRIVS
 * No /etc/passwd — SYSUAF is the user database.
 */
static void provision_sysuaf_users(void)
{
    char sysuaf_path[512];
    vms_to_linux(VMS_SYSUAF_PATH, sysuaf_path, sizeof(sysuaf_path));
    FILE *fp = fopen(sysuaf_path, "r");
    if (!fp)
        return;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *user, *defdir;
        uint32_t grp, mem;
        if (sysuaf_split(line, &user, &grp, &mem, &defdir) != 0)
            continue;
        if (defdir[0] == '\0')
            continue;

        /* default_dir may be a VMS spec (DKA0:[USERS.name]) — translate. */
        char home_linux[512];
        if (strchr(defdir, '[') || strchr(defdir, ':')) {
            vms_to_linux(defdir, home_linux, sizeof(home_linux));
        } else {
            /* Legacy Linux path — use directly */
            snprintf(home_linux, sizeof(home_linux), "%s", defdir);
        }
        mkdir(home_linux, 0755);
        own_object(home_linux, grp, mem);
    }
    fclose(fp);
}

/*
 * Give the system root ([SYS0] and everything under it) to the UIC that
 * OWNS the OpenVMS system tree: the SYSTEM account's.
 *
 * PINNED TO THE ORACLE, not chosen (CLAUDE.md Rule 10). Measured on VAX2
 * of the ~/vax/cluster lab, OpenVMS VAX V7.3, 30-JUL-2026 -- verbatim
 * transcripts in docs/oracle/vax73-privileges.md S7:
 *
 *   $ DIRECTORY/OWNER/PROTECTION SYS$COMMON:[000000]SYSEXE.DIR,SYSLIB.DIR
 *   SYSEXE.DIR;1         [SYSTEM]                         (RWE,RWE,RE,RE)
 *   SYSLIB.DIR;1         [SYSTEM]                         (RWE,RWE,RE,RE)
 *   $ DIRECTORY/OWNER/PROTECTION SYS$SYSTEM:LOGINOUT.EXE,AUTHORIZE.EXE
 *   LOGINOUT.EXE;1       [SYSTEM]                         (RWED,RWED,RWED,RE)
 *   $ DIRECTORY/OWNER/PROTECTION SYS$SYSDEVICE:[000000]*.DIR
 *   SYS0.DIR;1           [SYSTEM]                         (RWE,RWE,RE,RE)
 *
 * So on VMS the system tree is owned by SYSTEM and world gets R+E and no
 * write -- which is exactly the pair of facts OVMX has to reproduce: the
 * SYSTEM account can create and delete in SYS$SYSTEM: and SYS$MANAGER:,
 * and an ordinary user cannot.
 *
 * WHICH UIC IS NOT HARDCODED HERE. It is read out of SYSUAF's SYSTEM
 * record, the same record LOGINOUT authenticates against and the same one
 * establish_system_identity() below hands to the executive. If those two
 * ever disagreed, the file protection would be enforced against a
 * different identity than the executive holds -- which is the defect this
 * whole item exists to delete.
 *
 * WHY IT RUNS ON EVERY BOOT, not just at install: provision_seed_files()
 * adds files to the tree after the install step is skipped, and a file
 * seeded by root that SYSTEM cannot write is the same regression in a
 * smaller box.
 *
 * DISCLOSED DIVERGENCE (not a handler, a substrate limit): VMS grants the
 * SYSTEM protection category to every UIC whose group is <= MAXSYSGROUP
 * (measured 8, see S7). A Linux inode carries exactly one owning group, so
 * OVMX can express "UIC group 1 is the owner's group" but not "UIC groups
 * 1 through 8 are all system". Accounts in groups 2..8 would therefore get
 * VMS's GROUP nibble where VMS gives them the SYSTEM one. OVMX's SYSUAF
 * ships no such account, and inventing a second enforcement layer to paper
 * over it would be worse than the gap (Rule 10).
 */
static void provision_ownership(void)
{
    char sysuaf_path[512];
    vms_to_linux(VMS_SYSUAF_PATH, sysuaf_path, sizeof(sysuaf_path));
    FILE *fp = fopen(sysuaf_path, "r");
    if (!fp)
        return;

    uint32_t sys_grp = 0, sys_mem = 0;
    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *user, *defdir;
        uint32_t grp, mem;
        if (sysuaf_split(line, &user, &grp, &mem, &defdir) != 0)
            continue;
        if (strcmp(user, "SYSTEM") == 0) {
            sys_grp = grp;
            sys_mem = mem;
            found = 1;
            break;
        }
    }
    fclose(fp);

    /* No SYSTEM record means no authorized identity for the system
     * process at all; establish_system_identity() halts the boot on the
     * same condition a few steps later. Nothing to own here. */
    if (!found)
        return;

    /* [SYS0] -- the parent of VMS_SYSROOT ([SYS0.SYSCOMMON]) -- and
     * everything beneath it. */
    char path[512];
    vms_to_linux(VMS_SYSROOT, path, sizeof(path));
    char *last_slash = strrchr(path, '/');
    if (last_slash && last_slash != path)
        *last_slash = '\0';
    own_tree(path, sys_grp, sys_mem);
}

/*
 * Install the OVMX system onto the system disk (overlay mode).
 * Creates ODS-2 directory tree, populates [SYS0.SYSCOMMON.SYSEXE]
 * and [SYS0.SYSCOMMON.SYSLIB] with symlinks to discovered binaries,
 * and provisions SYSUAF user home directories.
 */
static void install_system(void)
{
    printf("%%STARTUP-I-INSTALL, installing OVMX system\n");
    provision_dirs();
    provision_symlinks();
    provision_sysuaf_users();
    printf("%%STARTUP-I-INSTALLED, system installation complete\n");
}

/*
 * Install the OVMX system onto a block-device system disk.
 * Copies real binaries from the initramfs backup to the mounted
 * block device. No symlinks — vmsfs block-device mode stores real files.
 */
static void install_system_blkdev(void)
{
    printf("%%STARTUP-I-INSTALL, installing OVMX system to DKA0:\n");
    provision_dirs();
    copy_recursive(INITRAMFS_BACKUP, SYSDISK_MOUNT);
    provision_sysuaf_users();
    printf("%%STARTUP-I-INSTALLED, system installation complete\n");
}

/*
 * NOTE ON SERVICES: STARTUP.EXE STARTS NO SERVICES. DO NOT ADD ONE HERE.
 *
 * It used to fork the SSH daemon (and, before that, the logical name
 * daemon) directly, which was wrong on three counts:
 *
 *   - the service was invisible to SYS$MANAGER, so it could not be
 *     enabled or disabled the VMS way;
 *   - it became an anonymous Linux child rather than a named detached
 *     process, so nothing could find it, and SHOW SYSTEM could not list
 *     it;
 *   - a missing service image was skipped SILENTLY, by the
 *     "if (stat(...) != 0) return -1;" this replaces. That is the bug
 *     class that let VMSSSHD.EXE go missing from the initramfs while the
 *     boot banner still announced SSH as running.
 *
 * Services start where VMS starts them:
 *
 *   STARTUP.COM -> SYSTARTUP_VMS.COM -> <service>_STARTUP.COM
 *                                       -> RUN/DETACHED/PROCESS_NAME=...
 *
 * which reaches $CREPRC with PRC$M_DETACH, so the executive names the
 * process and every other process can see it. Add a service to
 * SYS$MANAGER:SYSTARTUP_VMS.COM, never to this file (vms-47b).
 */

/*
 * Read one pipe-delimited field of a SYSUAF record by user name.
 * Returns 0 and fills out on success, -1 if there is no such user, no
 * such field, or SYSUAF.DAT cannot be read.
 */
static int sysuaf_field(const char *username, int index,
                        char *out, size_t outsz)
{
    char sysuaf_path[512];
    vms_to_linux(VMS_SYSUAF_PATH, sysuaf_path, sizeof(sysuaf_path));
    FILE *fp = fopen(sysuaf_path, "r");
    if (!fp)
        return -1;

    size_t namelen = strlen(username);
    char line[512];
    int found = -1;

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;
        if (strncmp(line, username, namelen) != 0 || line[namelen] != '|')
            continue;

        char *field = line;
        for (int i = 0; i < index; i++) {
            field = strchr(field, '|');
            if (!field) break;
            field++;
        }
        if (!field)
            break;

        char *end = field;
        while (*end && *end != '|' && *end != '\n' && *end != '\r')
            end++;
        *end = '\0';

        snprintf(out, outsz, "%s", field);
        found = 0;
        break;
    }

    fclose(fp);
    return found;
}

/*
 * Establish PID 1's identity through the executive (vms-2b8).
 *
 * WHAT THIS REPLACES, so it is never put back: PID 1 used to call
 *     vms_pcb_init(0xFFFFFFFFFFFFFFFFULL);
 *     vms_pcb_set_identity(1, (1 << 16) | 4, "SYSTEM", "SYSTEM");
 * -- a process writing its own user name, its own UIC and every
 * privilege bit in existence into a structure it owns privately. The
 * answer it wrote was the right one; WHO DECIDED IT was the defect
 * (CLAUDE.md Rule 11). Nothing outside PID 1 could see that PCB, and
 * nothing could refuse it.
 *
 * The identity now comes from the two places a VMS identity can come
 * from, and PID 1 supplies neither of them:
 *   - WHAT the identity is: SYSUAF's SYSTEM record -- the same user
 *     database LOGINOUT authenticates against, and the same one that
 *     already decides the UIC and privileges of every other session.
 *   - WHETHER PID 1 may establish it: the executive's own judgement.
 *     VMS_IOCTL_SETIDENT refuses any caller that does not hold SETPRV
 *     an identity that is not a weakening of its own, and SETPRV is
 *     derived in vms.ko from capable(CAP_SYS_ADMIN) at registration --
 *     a credential no process can grant itself.
 *
 * So this is LOGINOUT's shape, run for the system process: read the
 * authorized record, ask the executive to stamp it, and let the
 * executive say no. The row it writes is what every other process on
 * the node sees for PID 1, which is the whole point.
 *
 * FAILURE IS FATAL, and deliberately so. There is no VMS in which the
 * system process has no identity, so there is no VMS behaviour to
 * reproduce for that state and no plausible-looking default to pick
 * (Rule 10): the condition is made unreachable instead. The diagnostic
 * wears the OVMX facility, not a VMS one, because the event is OVMX's.
 */
static void establish_system_identity(void)
{
    char uic_group[32] = "", uic_member[32] = "", priv_str[256] = "";

    /* SYSUAF fields, 0-indexed and pipe-delimited, the same layout
     * provision_sysuaf_users() above walks:
     *   0 USERNAME | 1 HASH | 2 UIC_GROUP | 3 UIC_MEMBER
     *   4 DEFAULT_DIR | 5 FLAGS | 6 PRIVILEGES
     * Parsed here rather than through libvms's sysuaf_lookup() because
     * PID 1 must stay statically linked against the minimal set of
     * libraries (see the -static note at the bottom of this target's
     * CMakeLists.txt); it does not link libvms. */
    if (sysuaf_field("SYSTEM", 2, uic_group, sizeof(uic_group)) != 0 ||
        sysuaf_field("SYSTEM", 3, uic_member, sizeof(uic_member)) != 0 ||
        sysuaf_field("SYSTEM", 6, priv_str, sizeof(priv_str)) != 0)
        ovmx_exec_halt("no SYSTEM record in SYS$SYSTEM:SYSUAF.DAT",
                       "the system process has no authorized identity");

    /*
     * OCTAL (vms-e60). SYSUAF.DAT's UIC fields are octal; derivation in
     * src/libvms/rtl/sysuaf.c. This site is the one that mattered most and
     * the item did not name it: PID 1 parses SYSTEM's UIC here and stamps
     * it into the executive via vms_kif_setident(), so a base mismatch here
     * gives the system process a different identity from the one every
     * other reader of SYSUAF computes. It was correct only because SYSTEM
     * is 1|4, which reads the same in both bases.
     */
    uint32_t uic = ((uint32_t)strtoul(uic_group, NULL, 8) << 16) |
                    (uint32_t)strtoul(uic_member, NULL, 8);
    uint64_t privs = parse_privilege_string(priv_str);

    uint32_t st = vms_kif_setident("SYSTEM", uic, privs);
    if (!(st & 1)) {
        char detail[64];
        snprintf(detail, sizeof(detail), "SS$ status %u", (unsigned)st);
        ovmx_exec_halt("the executive refused the system process's identity",
                       detail);
    }

    /*
     * Seed the userspace PCB from the row the executive just wrote --
     * a copy of the executive's verdict, never a declaration. (The PCB
     * is per-process and therefore a facade for anything system-wide;
     * removing it is vms-8019's, not this item's. What this item
     * removes is PID 1 CHOOSING what goes in it.)
     */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    st = vms_kif_getjpi_self(&info);
    if (!(st & 1))
        ovmx_exec_halt("the executive holds no row for the system process",
                       "identity was stamped but cannot be read back");

    struct vms_pcb *pcb = vms_pcb_init(info.cur_privs);
    if (pcb) {
        vms_pcb_set_identity(info.vms_pid, info.uic, info.username,
                             info.prcnam);
        vms_pcb_set_default_dir("SYS$SYSROOT:[SYSMGR]");
    }

    printf("%%OVMX-I-EXEC, system identity %s [%o,%o] established by the executive\n",
           info.username, (unsigned)((info.uic >> 16) & 0xFFFFu),
           (unsigned)(info.uic & 0xFFFFu));
}

/*
 * Run SYS$MANAGER:STARTUP.COM via DCL subprocess.
 * Waits for completion before returning.
 */
static void run_startup(void)
{
    char startup_path[512], dcl_path[512];
    vms_to_linux(VMS_STARTUP_PATH, startup_path, sizeof(startup_path));
    vms_to_linux(VMS_DCL_PATH, dcl_path, sizeof(dcl_path));

    struct stat st;
    if (stat(startup_path, &st) != 0)
        return;

    pid_t pid = fork();
    if (pid == 0) {
        execl(dcl_path, "vmsdcl", startup_path, (char *)NULL);
        _exit(1);
    }
    if (pid > 0) {
        int s;
        waitpid(pid, &s, 0);
    }
}

/*
 * Display VMS-style boot banner.
 *
 * Announces only what STARTUP.EXE itself did. A service announces
 * itself from its own startup procedure, as VMS does -- this banner
 * must never claim a service is running, because STARTUP.EXE no longer
 * starts any and could not know.
 */
static void display_boot_banner(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    printf("\n");
    printf("    %s\n", ovmx_product_banner());
    printf("    %2d-%s-%04d %02d:%02d:%02d.%02d\n\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));

    printf("%%STDRV-I-STARTUP, OVMX startup begun at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));

    /* Re-read time for "completed" message */
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    printf("%%STDRV-I-STARTUP, OVMX startup completed at %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));
    printf("\n");
}

/*
 * Main: boot sequence then login loop.
 */
int main(void)
{
    /* If we are PID 1 on bare metal, set up Linux plumbing */
    struct stat bm_st;
    if (getpid() == 1 && stat("/proc/version", &bm_st) != 0) {
        bare_metal_init();
    }

    /* No image runs without the executive. On bare metal this is already
     * satisfied (bare_metal_init attached it before INITIALIZE.EXE ran) and
     * this call is a no-op; on any other substrate it is the gate. Placing
     * it here, unconditionally, is deliberate: a boot path that skips the
     * Linux plumbing must not thereby skip the executive. */
    executive_attach();

    /* Set up signal handling */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Reap zombies when running as PID 1 */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);

    /* Ignore SIGHUP so login children can terminate without killing us */
    signal(SIGHUP, SIG_IGN);

    /*
     * Step 1: SYSTEM process identity.
     *
     * WHAT USED TO STAND HERE (vms-2b8, closed this round): PID 1 called
     *     vms_pcb_init(0xFFFFFFFFFFFFFFFFULL);
     *     vms_pcb_set_identity(1, (1 << 16) | 4, "SYSTEM", "SYSTEM");
     * -- a process writing its own user name, its own UIC and every
     * privilege bit in existence into a structure it owns privately.
     * Nothing outside PID 1 could see that PCB, and nothing could refuse
     * it. The blocking precondition this stopgap was waiting on
     * (vms_kif_setident() binding before its ioctl, vms-fb9 r6) has
     * landed, so the real establishment now runs: see
     * establish_system_identity() below, called from Step 2c after
     * SYSUAF exists on the system disk. It reads PID 1's identity from
     * SYSUAF's SYSTEM record and asks the executive to stamp it, and the
     * executive can refuse -- exactly LOGINOUT's shape, run for the
     * system process.
     */

    /* Step 1b: Bootstrap VMS namespace — device table + logical names.
     * This is the bridge: one Linux mount point enters the device table,
     * and from this point forward all paths are VMS filespecs translated
     * at point of use. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);
    init_search_paths();

    /* Step 2: Install system if not already on the system disk */
    if (is_system_installed()) {
        printf("%%STARTUP-I-SYSBOOT, system disk detected, skipping install\n");
    } else {
        if (blkdev_mode)
            install_system_blkdev();
        else
            install_system();
    }

    /* Step 2b: Ensure seed data files exist on system disk.
     * On first boot, install copies the whole tree but subsequent
     * boots skip install. Seed files that were added after install
     * (e.g., RIGHTSLIST.DAT, STARTUP.COM) are provisioned here
     * without overwriting existing files. */
    provision_seed_files();

    /* Step 2b': Give the VMS tree the ownership VMS gives it, so that the
     * identity the executive enforces and the identity the filesystem
     * enforces are the same one (vms-2b8). Runs after the seed files
     * exist and after any install, and is the LAST writer of ownership so
     * that accounts sharing a directory (SYSUAF ships SYSTEM and OPERATOR
     * both defaulted to [SYSMGR]) cannot leave the system tree owned by
     * whichever record was read last. */
    provision_ownership();

    /* Step 2c: Establish PID 1's identity — from SYSUAF, through the
     * executive. Must run after provision_seed_files(): SYSUAF.DAT is
     * one of the files it seeds, so on a first boot there is no user
     * database before this point. */
    establish_system_identity();

    /* Step 3: Run STARTUP.COM.
     * There is no logical name daemon to start first: on VMS the logical name
     * tables are executive-resident, not a service (vms-a4b).
     *
     * This is ALSO where the system's services start, and the only place
     * they do: STARTUP.COM invokes SYSTARTUP_VMS.COM, which invokes each
     * service's own startup procedure, which creates it with
     * RUN/DETACHED under a VMS process name (vms-47b). STARTUP.EXE
     * deliberately starts none of its own -- see the NOTE ON SERVICES
     * above. */
    run_startup();

    /* Step 4: Boot banner */
    display_boot_banner();
    fflush(stdout);
    fflush(stderr);

    /* Step 5: Login loop (only if stdin is a terminal) */
    char loginout_path[512], dcl_path[512];
    vms_to_linux(VMS_LOGINOUT_PATH, loginout_path, sizeof(loginout_path));
    vms_to_linux(VMS_DCL_PATH, dcl_path, sizeof(dcl_path));

    int console_interactive = isatty(STDIN_FILENO);
    int consecutive_failures = 0;

    /* There is no "wait forever because a service is serving sessions"
     * branch here any more. It existed for the SSH daemon STARTUP.EXE
     * used to fork, and it could only ever be entered on the dead Docker
     * runtime (CLAUDE.md Rule 9). With no console, the loop below exits
     * on EOF exactly as it always did when no daemon was running. */

    while (!shutdown_requested) {
        /* Check if stdin is still open (avoid tight loop on EOF) */
        if (!console_interactive && feof(stdin)) {
            break;
        }

        struct timespec t_before;
        clock_gettime(CLOCK_MONOTONIC, &t_before);

        pid_t child = fork();
        if (child == 0) {
            /*
             * DELETED, NOT REPLACED (vms-fb9): setenv("VMS_TERMINAL",
             * "_OPA0:", 1) stood here. PID 1 told its login child what
             * terminal it was on through the environment -- the rejected
             * VMS_PRCNAM shape (CLAUDE.md rule 10, worked example 2). It
             * was not even a claim anything could check: the child had no
             * way to verify it and no other process could see it.
             *
             * OPA0: IS real -- the executive creates it at module init
             * (src/kernel/vms_devtab.c) and every process on the node can
             * read it. So the console terminal does not need to be
             * announced; it needs to be LOOKED UP, with $ASSIGN and
             * $GETDVI on the resulting channel. PID 1 has no business
             * asserting it, and nothing downstream may be built on this
             * line being here.
             *
             * WHAT STANDS HERE INSTEAD (vms-d0b). The login session takes
             * a real channel to the console and asks the executive to
             * record that channel's device as this job's terminal. Three
             * properties, and each is the reason the environment variable
             * was not simply reinstated behind a function call:
             *
             *   - The name is not transmitted. $ASSIGN names the console
             *     because PID 1 is CREATING A SESSION ON IT -- that is
             *     system configuration, the same way DKA0: is named at
             *     step 1b -- but VMS_IOCTL_SETTERM takes only the
             *     CHANNEL. The executive reads the device off the channel
             *     it issued and copies its own name. Nothing downstream
             *     receives a string it must trust.
             *   - The binding is in the executive, so a DIFFERENT process
             *     can read which terminal this job is on ($GETJPI), which
             *     is what makes it a fact rather than a self-description
             *     (CLAUDE.md Rule 11).
             *   - It survives the execl() below. The executive keys the
             *     process table on the thread-group id, which execve()
             *     does not change, so LOGINOUT.EXE and then DCL.EXE run
             *     with the binding their process already has, carrying
             *     nothing.
             *
             * Neither status is examined, deliberately, and this is the
             * same reasoning as cmd_show_device()'s untested
             * vms_kif_open(): the conditions they could report are ones
             * OVMX is not in. The executive is pinned open for the life
             * of the system (executive_attach, above, which halts if it
             * is absent), OPA0: is created at module init and vms.ko
             * implements no operation that removes a device, and the
             * channel handed to SETTERM is the one $ASSIGN just returned.
             * A branch here would be a handler for a state VMS is not in
             * (Rule 10), and the only thing it could usefully do is
             * fabricate a binding.
             * If a call did fail, the executive records no terminal --
             * and SHOW TERMINAL then names none, which is the honest
             * outcome and the one the reader already renders.
             */
            uint32_t console_chan = 0;
            (void)vms_kif_assign(OVMX_CONSOLE_DEVICE, &console_chan);
            (void)vms_kif_setterm(console_chan);

            /* Child: exec vms_login (SYS$SYSTEM:LOGINOUT.EXE). */
            execl(loginout_path, "vms_login", (char *)NULL);

            /*
             * NO DCL FALLBACK (vms-72c). "exec vmsdcl directly" used to
             * stand here if the LOGINOUT.EXE exec above failed -- an
             * unauthenticated shell handed to whoever is at the console,
             * reached by nothing more than a missing or unexecutable
             * file. That is CLAUDE.md Rule 10's illegal third answer,
             * named for exactly this shape in this item's own dispatch
             * text: VMS has no state in which the console driver cannot
             * run LOGINOUT and responds by starting an interactive
             * session anyway with no username, no password and no
             * SYSUAF check. It is also the same defect this item closes
             * one line earlier for the empty-password-hash SYSUAF
             * shipped by default (distro/rootfs/.../SYSUAF.DAT) --
             * a second path to the identical outcome, "session reached
             * with no real authentication", would have made that fix
             * partial.
             *
             * MADE UNREACHABLE, NOT HANDLED, per Rule 10's other answer:
             * LOGINOUT.EXE is a required system file, provisioned onto
             * every system disk by provision_symlinks() (this file,
             * "install once, boot forever" -- see is_system_installed())
             * before the login loop below can ever run, so failing to
             * exec it here is the same class of condition as vms.ko or
             * /dev/vms being absent (executive_attach(), above) --
             * OVMX's one runtime does not come up in that state. Unlike
             * the executive gate, the response here is not to halt the
             * whole boot: this is a per-login-attempt failure, not a
             * per-system one, and the outer loop already retries with
             * backoff (see "consecutive_failures" below) instead of
             * surrendering the console -- NOT independently oracle-pinned
             * here as "what VMS's console driver does on an image
             * activation failure" (the ~/vax lab was unavailable for this
             * item, mid-use for an unrelated experiment); it is the
             * behavior this loop already had for every other login
             * failure before this item touched it, kept unchanged. So the
             * child reports why (OVMX facility, not a
             * VMS one -- a Linux exec(2) failure has no VMS analogue,
             * same reasoning as ovmx_exec_halt above) and exits, and the
             * loop tries again; what it may not do is substitute an
             * unauthenticated shell for the login it could not run.
             */
            fprintf(stderr, "%%OVMX-E-NOLOGIN, cannot exec %s: %s\n",
                    VMS_LOGINOUT_PATH, strerror(errno));
            _exit(1);
        } else if (child > 0) {
            /* Parent: wait for login session to end */
            int wstatus;
            waitpid(child, &wstatus, 0);

            struct timespec t_after;
            clock_gettime(CLOCK_MONOTONIC, &t_after);
            long elapsed_ms = (t_after.tv_sec - t_before.tv_sec) * 1000
                            + (t_after.tv_nsec - t_before.tv_nsec) / 1000000;

            /* Track consecutive fast failures (< 1 second) */
            if (elapsed_ms < 1000) {
                consecutive_failures++;
                if (consecutive_failures >= 5) {
                    fprintf(stderr,
                        "%%STARTUP-F-LOGINFAIL, login process failing repeatedly\n");
                    if (WIFEXITED(wstatus))
                        fprintf(stderr,
                            "%%STARTUP-F-LOGINFAIL, exit status %d\n",
                            WEXITSTATUS(wstatus));
                    else if (WIFSIGNALED(wstatus))
                        fprintf(stderr,
                            "%%STARTUP-F-LOGINFAIL, killed by signal %d\n",
                            WTERMSIG(wstatus));

                    /* Check if the binaries actually exist (VMS specs in messages) */
                    struct stat chk;
                    fprintf(stderr, "%%STARTUP-I-DIAG, %s: %s\n",
                            VMS_LOGINOUT_PATH,
                            stat(loginout_path, &chk) == 0 ?
                                "exists" : strerror(errno));
                    fprintf(stderr, "%%STARTUP-I-DIAG, %s: %s\n",
                            VMS_DCL_PATH,
                            stat(dcl_path, &chk) == 0 ?
                                "exists" : strerror(errno));

                    /* Back off instead of spinning */
                    sleep(5);
                    consecutive_failures = 0;
                }
            } else {
                consecutive_failures = 0;
            }

            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
                usleep(100000);
            }

            /* Print blank line between sessions (like real VMS console) */
            printf("\n");
            fflush(stdout);
        } else {
            /* fork failed */
            perror("fork");
            sleep(1);
        }
    }

    /* No daemons to clean up: STARTUP.EXE started none. A detached
     * process created by a startup procedure is NOT STARTUP.EXE's child
     * and cannot be waited on by it -- shutting one down is $DELPRC by
     * process name, which belongs to a shutdown procedure, not here. */
    return 0;
}
