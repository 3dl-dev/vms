/*
 * ovmx_init.c - OVMX Boot Orchestrator (STARTUP.EXE)
 *
 * PID 1 / ENTRYPOINT for OVMX on its one runtime target: the real-kernel /
 * QEMU path (CLAUDE.md Rule 9). Handles the entire boot sequence: filesystem
 * setup, kernel module loading, VMS directory provisioning, boot banner,
 * and console login loop.
 *
 * IT DOES NOT READ SYS$SYSTEM:SYSUAF.DAT (vms-9b7). Account provisioning,
 * system-tree ownership and the SYSTEM identity are system management, and
 * run from SYS$MANAGER:STARTUP.COM in PROVISION.EXE -- see the Step 1 note
 * in main(). No shell scripts, no busybox, no /etc/passwd.
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

/* vms/pcb.h and vms/privs.h are DELIBERATELY NOT INCLUDED (vms-9b7). PID 1
 * no longer seeds a private PCB and no longer parses a privilege string --
 * both were part of reading SYSUAF here, which it no longer does. */
#include "vms/logical.h"
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
 * SYS$SYSTEM / SYS$LIBRARY as Linux paths — initialized at runtime after
 * the device table is populated so VMS specs can be translated.
 */
static char sysexe_linux[512];
static char syslib_linux[512];

static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static volatile sig_atomic_t shutdown_requested = 0;

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
 * DCL.EXE in SYSEXE is the marker — if the file exists, a prior install
 * (or the initramfs backing itself, in overlay mode) populated the tree
 * and we can skip straight to boot.
 */
static int is_system_installed(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/DCL.EXE", sysexe_linux);
    struct stat st;
    return (stat(path, &st) == 0);
}

/*
 * SYSTEM's UIC, as a fixed OVMX substrate convention -- NOT a SYSUAF read
 * (vms-e5c).
 *
 * [USERS] and [SYSTMP] are MFD-level siblings of [SYS0] (see the tree below),
 * so PROVISION.EXE's provision_ownership() walk (src/ovmx_provision/
 * ovmx_provision.c, which chowns [SYS0] and everything beneath it to
 * whatever UIC SYSUAF's SYSTEM record carries once PID 1 has handed off)
 * never reaches them. This file mkdir()s them at boot, as root, and until
 * now they stayed root:root 0755 forever -- so a SYSTEM (UIC [1,4]) DCL
 * session got EACCES on sys$create under SYS$SCRATCH: ([SYSTMP]) or the
 * [USERS] area, and OVMX's own PARTS demo silently dropped all the way to
 * its /tmp fallback candidate. That is a Unix leak in a "zero Unix leaks"
 * narrative, and the root cause: no user-writable VMS directory existed
 * for SYSTEM to fall into.
 *
 * WHY THIS IS A FULL [UIC_GROUP, UIC_MEMBER] MATCH, NOT A GROUP-ONLY ONE
 * (MEASURED, vms-e5c). vmsfs's real access decision
 * (src/kernel/vmsfs/vmsfs_blkdev.c, vmsfs_blkdev_permission()) is VMS SOGW,
 * not Unix rwx: every new file/directory is stamped VMSFS_PROT_DEFAULT
 * (0xAA00) at creation, which denies WRITE to the System and World
 * categories alike, exactly like a real VMS default (S:RWED,O:RWED,G:RE,
 * W:RE) -- and it is a per-inode field the kernel module controls,
 * completely independent of the Linux mode bits a plain chmod(2) sets. An
 * earlier version of this fix chowned the GROUP only and left owner as
 * root, banking on a Linux-style "group-writable" bit; MEASURED under a
 * real QEMU boot with the real executive, that gave SYSTEM (UIC [1,4],
 * i.e. euid=4/egid=1 after LOGINOUT's credential drop) the GROUP category
 * -- and GROUP is denied WRITE by VMSFS_PROT_DEFAULT precisely because
 * that is what "group" access means by default on real VMS too. Only the
 * OWNER category (BOTH euid AND egid matching the inode, per
 * vmsfs_blkdev_permission()'s own check) escapes that default denial. So
 * SYSTEM has to be made the actual owner -- [UIC_MEMBER, UIC_GROUP] BOTH
 * -- not merely share its group.
 *
 * PID 1 does not read SYSUAF and must not grow a second opinion about who
 * SYSTEM is (vms-9b7) -- that was two independent 512-byte parsers of one
 * file disagreeing about a SYSTEM row, and the fix was that PID 1 stopped
 * parsing it at all. So these values are NOT derived from a per-installation
 * data file at boot; they are the fixed UIC the shipped
 * distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT gives the SYSTEM
 * account ("SYSTEM|...|1|4|..."), the same [1,4] this project treats as a
 * standing convention elsewhere (e.g. the pre-vms-9b7 PID 1 stamped this
 * exact pair before the executive became the source of truth for it). If a
 * future SYSUAF ever moves SYSTEM to a different UIC, these constants have
 * to move with it -- disclosed here so the coupling is not silently
 * rediscovered as a boot-time mismatch.
 */
#define OVMX_SYSTEM_UIC_GROUP   1
#define OVMX_SYSTEM_UIC_MEMBER  4

/*
 * mkdir a directory that SYSTEM must be able to sys$create into, and make
 * SYSTEM its actual owner -- both UIC fields, not just the group (see the
 * MEASURED note above for why group-only is not enough on vmsfs). This is
 * an OVMX substrate-limit design choice, already established and labelled
 * as such in provision_ownership()'s "DISCLOSED DIVERGENCE" note -- not
 * presented as a VMS-authentic mechanism (on real VMS this area would stay
 * SYSTEM-owned by the same account-provisioning step that creates every
 * other SYSTEM-owned system directory; here it is PID 1, at first boot,
 * stamping a fixed, disclosed UIC rather than an identity it looked up).
 * chmod to 0775 as well so DIRECTORY/PROTECTION's Linux-mode fallback
 * display (vmsfs_mode_to_protection) is not left claiming a stricter
 * protection than what vms_prot's OWNER category (VMSFS_PROT_DEFAULT's
 * zero-deny nibble) actually grants -- it does not change enforcement, only
 * what a mode-based reader sees, since vmsfs's read/write DECISION never
 * consults i_mode (see the MEASURED note above).
 */
static void provision_writable_dir(const char *path)
{
    mkdir(path, 0775);
    if (chown(path, (uid_t)OVMX_SYSTEM_UIC_MEMBER,
              (gid_t)OVMX_SYSTEM_UIC_GROUP) != 0 && errno != ENOENT)
        fprintf(stderr, "%%STARTUP-W-OWNER, cannot set owner of %s: %s\n",
                path, strerror(errno));
    if (chmod(path, 0775) != 0 && errno != ENOENT)
        fprintf(stderr, "%%STARTUP-W-OWNER, cannot set mode of %s: %s\n",
                path, strerror(errno));
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
 * [USERS]               /vms/USERS/         -- SYSTEM-writable (vms-e5c)
 * [SYSTMP]              /vms/SYSTMP/        -- SYSTEM-writable (vms-e5c)
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
    provision_writable_dir(path);
    vms_to_linux(VMS_SYSTMP, path, sizeof(path));
    provision_writable_dir(path);

    mkdir("/tmp/ovmx", 0755);
    mkdir("/tmp/ovmx/locks", 0755);
}

/*
 * Install the OVMX system onto the system disk.
 *
 * Creates the ODS-2 directory tree, then copies the real files from the
 * initramfs backup (INITRAMFS_BACKUP, populated by bare_metal_init before
 * the system disk was mounted over it) onto the now-mounted system disk.
 *
 * IT DOES NOT PROVISION ACCOUNTS, AND MUST NOT (vms-9b7). This used to end
 * with provision_sysuaf_users(), which read SYSUAF.DAT and created and
 * chowned each account's home directory. Creating accounts' directories is
 * ACCOUNT PROVISIONING -- on OpenVMS it is AUTHORIZE's job, driven from a
 * DCL procedure -- and it is the same shape as the services this function's
 * own NOTE below refuses to start here. It now runs from SYS$MANAGER:
 * STARTUP.COM, in PROVISION.EXE, which is an ordinary image linking the
 * ordinary libraries. This function copies files; that is all it does.
 *
 * Only reached when is_system_installed() found no DCL.EXE on the mounted
 * disk — i.e. a blank block-device disk just initialized by INITIALIZE.EXE.
 * In overlay mode the backing store is the initramfs copy itself, so
 * DCL.EXE is already there and this function is never called.
 *
 * No symlinks: this is a real copy onto real vmsfs storage, so binaries
 * and libraries land at their VMS paths as real files, not indirection.
 */
static void install_system(void)
{
    printf("%%STARTUP-I-INSTALL, installing OVMX system to DKA0:\n");
    provision_dirs();
    copy_recursive(INITRAMFS_BACKUP, SYSDISK_MOUNT);
    /* Every file just written sits dirty in the page cache until the
     * kernel's own periodic writeback runs (default ~30s). A user who
     * reaches the login prompt below and quits QEMU before that timer
     * fires (the documented "log out or Ctrl-A X here" step) loses the
     * install: the next boot reads a system disk that was never actually
     * written. Force it out now, while install_system() still knows the
     * write is what must survive.
     *
     * SYSUAF.DAT is among those files, and that is the ONLY relationship
     * this sync() has to it now (vms-9b7): install_system() copies it and
     * never reads it. Account provisioning moved to PROVISION.EXE. */
    sync();

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
 * Run the startup process.
 *
 * PID 1 EXECS PROVISION.EXE, NOT DCL.EXE (vms-9b7). PROVISION.EXE establishes
 * the SYSTEM identity from SYSUAF through the executive, provisions the system
 * tree's ownership and the accounts' home directories, and then execs DCL.EXE
 * on SYS$MANAGER:STARTUP.COM in the SAME process -- so the DCL that runs
 * STARTUP.COM and SYSTARTUP_VMS.COM holds SYSTEM's identity, which is what
 * OpenVMS does (STARTUP runs under username SYSTEM) and what this file used to
 * approximate by stamping PID 1 instead.
 *
 * The three steps that used to run HERE, in PID 1, before this fork --
 * provision_ownership(), establish_system_identity() and (from
 * install_system()) provision_sysuaf_users() -- are the same three steps, in
 * the same order, moved into that image. What is gone with them is PID 1's own
 * pair of hand-rolled SYSUAF parsers.
 *
 * A MISSING OR FAILING STARTUP PROCESS IS FATAL, and it is checked here rather
 * than skipped. The version of this function that only stat()ed STARTUP.COM
 * and silently returned is the bug class named in the NOTE ON SERVICES above:
 * a boot that quietly does not establish an identity, with a banner that says
 * everything is fine.
 */
static void run_startup(void)
{
    char provision_path[512];
    vms_to_linux(VMS_PROVISION_PATH, provision_path, sizeof(provision_path));

    struct stat st;
    if (stat(provision_path, &st) != 0)
        ovmx_exec_halt("SYS$SYSTEM:PROVISION.EXE is missing",
                       "the system process has no authorized identity");

    pid_t pid = fork();
    if (pid == 0) {
        execl(provision_path, "PROVISION", (char *)NULL);
        fprintf(stderr, "%%OVMX-E-NOIMG, cannot activate %s: %s\n",
                VMS_PROVISION_PATH, strerror(errno));
        _exit(1);
    }
    if (pid > 0) {
        int s;
        waitpid(pid, &s, 0);
        /*
         * PROVISION.EXE powers the machine off itself on the conditions that
         * are fatal by design (no SYSTEM record, executive refusal), so
         * reaching here with a nonzero status means it could not even do
         * that -- e.g. the image would not activate. Halt rather than fall
         * through to a login prompt on a system with no identity.
         */
        if (!WIFEXITED(s) || WEXITSTATUS(s) != 0)
            ovmx_exec_halt("the startup process did not complete",
                           "SYS$SYSTEM:PROVISION.EXE failed");
    }
}

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
     * Step 1: PID 1 DOES NOT READ SYS$SYSTEM:SYSUAF.DAT. AT ALL.
     *
     * WHAT USED TO STAND HERE, in two successive forms, so neither goes
     * back:
     *
     *   (1) PID 1 declared its own identity --
     *           vms_pcb_init(0xFFFFFFFFFFFFFFFFULL);
     *           vms_pcb_set_identity(1, (1 << 16) | 4, "SYSTEM", "SYSTEM");
     *       a process writing its own user name, its own UIC and every
     *       privilege bit in existence into a structure it owns privately.
     *       Nothing outside PID 1 could see that PCB and nothing could
     *       refuse it (vms-2b8, Rule 11).
     *
     *   (2) PID 1 read SYSUAF's SYSTEM record itself and asked the executive
     *       to stamp it (establish_system_identity(), Step 2c). That fixed
     *       WHO DECIDES -- the executive can refuse -- but it left a
     *       bootstrap process parsing an account database, through TWO
     *       hand-rolled 512-byte parsers of its own (sysuaf_split() and
     *       sysuaf_field()) because PID 1 is statically linked. Those were
     *       two of FIVE independent parsers of one file format, with three
     *       different line limits between them, and the disagreement was
     *       fatal: MEASURED on a real QEMU boot (vms-9b7), a SYSTEM row whose
     *       sixth field separator fell past byte 511 was read here as a
     *       five-field record, and this file reported
     *           %OVMX-F-EXECINIT, no SYSTEM record in SYS$SYSTEM:SYSUAF.DAT
     *       and powered the machine off -- while every 1024-byte reader in
     *       the tree read the same row without complaint.
     *
     * THE ANSWER WAS NOT A BIGGER BUFFER OR A SHARED STATIC LIBRARY. It was
     * that PID 1 NEEDS SYSUAF FOR NOTHING. Its three uses -- account home
     * directories, system-tree ownership, and the system identity -- are all
     * system management, not bootstrap, and they now run from
     * SYS$MANAGER:STARTUP.COM in PROVISION.EXE, exactly where the NOTE ON
     * SERVICES below already sends every other piece of VMS-domain work that
     * accumulated in this file because this file was the only thing running.
     *
     * The minimum bootstrap to reach an activatable image is below, and none
     * of it parses anything: mounts and the executive (bare_metal_init /
     * executive_attach, above), the device table, the install-time FILE COPY,
     * and the logical names that make SYS$SYSTEM: and SYS$SHARE: resolve.
     *
     * PID 1'S OWN IDENTITY is now whatever the executive derived for it from
     * its real credentials at registration -- which is honest, and is the
     * only thing PID 1 is entitled to say about itself. It declares nothing.
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
        install_system();
    }

    /* Step 2b: Ensure seed data files exist on system disk.
     * On first boot, install copies the whole tree but subsequent
     * boots skip install. Seed files that were added after install
     * (e.g., RIGHTSLIST.DAT, STARTUP.COM) are provisioned here
     * without overwriting existing files. */
    provision_seed_files();

    /* Steps 2b' and 2c -- system-tree ownership and the SYSTEM identity --
     * USED TO STAND HERE and have moved to SYS$MANAGER:STARTUP.COM
     * (PROVISION.EXE). See the Step 1 note above. Nothing between
     * provision_seed_files() and run_startup() reads SYSUAF any more, and
     * nothing here may start doing so again. */

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
             * every system disk by install_system() (this file,
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
