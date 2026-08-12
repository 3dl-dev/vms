/*
 * ovmx_init.c - OVMX Boot Orchestrator (STARTUP.EXE)
 *
 * PID 1 / ENTRYPOINT for OVMX on its one runtime target: the real-kernel /
 * QEMU path (CLAUDE.md Rule 9). This is SYSBOOT + EXEC_INIT + SYSINIT and
 * NOTHING ELSE (docs/design-init-scope.md, operator ruling 2026-08-10 "STRIP
 * ALL OF IT"): mount the Linux substrate, attach the executive, load vmsfs.ko,
 * mount the system disk -- or halt -- hand off to STARTUP.COM, then wait.
 *
 * IT DOES NOT INSTALL, INITIALIZE OR PROVISION ANYTHING. A booting VMS system
 * FINDS its system disk already installed or does not boot (design-init-scope.md
 * §1): nothing in the VMS boot chain creates the directory tree, copies system
 * files, seeds SYSUAF, re-owns the tree, or initializes a blank volume. Those
 * are VMSINSTAL / PCSI / INITIALIZE / AUTHORIZE -- operator commands run once on
 * a system that is NOT the one booting. OVMX puts them in the installer spine
 * (vms-791 kit-master -> vms-8ab mastered disk image -> vms-df9 PCSI), never
 * here. PID 1 mounts the pre-installed disk or halts honestly (no overlay, no
 * auto-INITIALIZE, no first-boot install).
 *
 * IT DOES NOT READ SYS$SYSTEM:SYSUAF.DAT (vms-9b7). Account provisioning,
 * system-tree ownership and the SYSTEM identity are system management, and
 * run from SYS$MANAGER:STARTUP.COM in PROVISION.EXE -- see the Step 1 note
 * in main(). No shell scripts, no busybox, no /etc/passwd.
 *
 * IT DOES NOT RUN THE CONSOLE LOGIN LOOP (vms-8d2, design-init-scope.md §2
 * row "login loop"). On VMS the interactive console session is owned by
 * JOB_CONTROL, a DETACHED process STARTUP.COM's site startup creates --
 * never by the SYSINIT-equivalent bootstrap process. That loop now lives in
 * src/ovmx_job_control/ovmx_job_control.c (JOB_CONTROL.EXE), created by
 * SYS$STARTUP:JOB_CONTROL_STARTUP.COM (invoked from SYS$MANAGER:
 * SYSTARTUP_VMS.COM, run_startup() below) with RUN/DETACHED/PROCESS_NAME=
 * JOB_CONTROL -- the same vms-47b mechanism every other OVMX service uses.
 * PID 1's job ends when STARTUP.COM returns; it then just waits, because
 * Linux's PID 1 cannot exit without panicking the kernel, and there is no
 * VMS analogue of that requirement to satisfy instead.
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
#include "dcdef.h"
/* PID 1's identity is established THROUGH the executive, not declared. */
#include "vms_kif.h"
/* SYS$SYSTEM:OVMXVMSSYS.PAR reader (vms-b6a7) -- the SAME header vms_sysgen.c,
 * scsd.c, and the DCL lexicals already use (sysgen_read_string/sysgen_read_
 * param). PID 1 adds NO second parser (vms-9b7 is exactly that defect
 * class): it links vmsfs statically already (CMakeLists.txt's vmsfs_static
 * in OVMX_IMGACT mode), which is all this header-only reader needs. */
#include "sysgen_params.h"
/* SYSBOOT> conversational-boot prompt (vms-b81) -- operates on the SAME
 * SYS$SYSTEM:OVMXVMSSYS.PAR, just resolved to a raw Linux directory
 * (VMS_SYSTEM_DIR) instead of through vmsfs_to_linux_path(), because it has
 * to run before the device table exists -- see sysboot.h and
 * bare_metal_init() below. */
#include "sysboot.h"

#define SYSDISK_DEV      "/dev/vda"

/*
 * SYS$SYSTEM as a Linux path — initialized at runtime after the device table
 * is populated so VMS specs can be translated. Used only to confirm the mounted
 * system disk is a properly installed volume (DCL.EXE present) before handing
 * off to STARTUP.COM.
 */
static char sysexe_linux[512];

static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static volatile sig_atomic_t shutdown_requested = 0;

/*
 * Forward declarations (vms-1fb): print_banner_once() -- defined near
 * main(), below, next to display_boot_banner() -- is called from
 * bare_metal_init(), which is defined earlier in this file. See
 * display_boot_banner()'s own header comment for why the banner call moved
 * this early in the boot.
 */
static void print_banner_once(void);

/*
 * A SYSBOOT> conversational session's result (vms-b81), stashed by
 * bare_metal_init() so read_boot_parameters() -- which runs later, in
 * main(), once the device table exists -- uses THIS in-memory parameter
 * set instead of re-reading SYS$SYSTEM:OVMXVMSSYS.PAR from disk. That
 * re-read is exactly what VMS persistence semantics forbid here: SET
 * inside the prompt is in-memory-only for this boot, and re-reading the
 * file afterward would silently discard it (the file was never written
 * unless the operator ran WRITE). A flagless boot never sets this and
 * read_boot_parameters() falls through to its original, unchanged path.
 */
static int conversational_boot_result_valid = 0;
static struct sysgen_file conversational_boot_params;

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
 * SYSINIT stage halt: the system disk is not there, will not mount, or is not
 * a properly installed system volume. This is the "finds them or does not
 * boot" condition (design-init-scope.md §1): VMS's SYSINIT mounts the system
 * disk and cannot proceed without it, and OVMX's stripped PID 1 does NOT
 * install, initialize or fall back to an ephemeral overlay -- installation is
 * the installer spine's job, not the booting kernel's (operator ruling
 * 2026-08-10). There is NO oracle-pinned VMS status for a failed system-disk
 * mount, so this wears the OVMX facility (named for the correct VMS stage,
 * SYSINIT), never a fabricated VMS message (Rule 10) -- the same shape as
 * ovmx_exec_halt above. Halting is fail-honest, the opposite of the silent
 * overlay fallback this replaces.
 */
static void ovmx_sysinit_halt(const char *what, const char *detail)
{
    fprintf(stderr, "%%OVMX-F-SYSINIT, %s\n", what);
    if (detail)
        fprintf(stderr, "%%OVMX-I-SYSINIT, %s\n", detail);
    halt_now();
}

/*
 * Pre-create every disk unit's mount point directory, as root, before any
 * VMS session -- which runs de-privileged under its SYSUAF UIC (LOGINOUT
 * setuid()/setgid()'s every session onto it, tools/vms_login.c) -- ever
 * needs one (vms-651).
 *
 * WHY THIS HAS TO HAPPEN HERE, not in DCL. mkdir(2) under a directory's
 * parent needs write+search there for the CALLING process's uid/gid; a VMS
 * session's Linux uid IS its SYSUAF UIC member number, not root (VMS
 * privilege is not Linux capability -- see the vms_kif_disk_mount() comment
 * in src/kernel/vms_devtab.c for the other half of this), so it cannot
 * create a NEW directory under /mnt (root-owned, mode 0755) no matter what
 * VMS privilege it holds. PID 1 is the one thing on the node still root at
 * this point in boot, so it is the one thing that can create these
 * directories -- MOUNT (src/vmsdcl/dcl_cmd_misc.c) only ever targets a path
 * that already exists.
 *
 * The disk units come from the executive's own enumeration
 * (vms_kif_devscan(), src/kernel/vms_devtab.c -- vda -> DKA0:, vdb ->
 * DKA100:, ...), not a second, independent scan of /dev (Rule 11): PID 1
 * asks the SAME table MOUNT will ask.
 */
static void provision_disk_mount_points(void)
{
    struct vms_devinfo info;
    uint32_t index = 0;

    mkdir("/mnt", 0755);

    while (vms_kif_devscan(&index, &info) == SS$_NORMAL) {
        if (info.devclass != DC$_DISK)
            continue;

        char name[VMS_DEVNAM_SIZE];
        strncpy(name, info.devnam, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        size_t len = strlen(name);
        if (len > 0 && name[len - 1] == ':')
            name[len - 1] = '\0';
        for (size_t i = 0; name[i]; i++)
            name[i] = (char)tolower((unsigned char)name[i]);

        char mount_point[64];
        snprintf(mount_point, sizeof(mount_point), "/mnt/%s", name);
        mkdir(mount_point, 0755);
    }
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
 * Bare-metal bootstrap: mount the Linux substrate, set hostname, attach the
 * executive, load vmsfs.ko, and MOUNT THE SYSTEM DISK OR HALT. Called when
 * running as PID 1 on bare metal or QEMU.
 *
 * This is SYSINIT: it mounts the pre-installed system disk. It does NOT
 * install, initialize, or fall back to an ephemeral overlay -- a booting VMS
 * system finds its system disk or does not boot (design-init-scope.md §1,
 * operator ruling 2026-08-10). A disk that is absent or will not mount is a
 * fail-honest halt (ovmx_sysinit_halt), never a silent overlay onto a tmpfs
 * tree the way the deleted code did.
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

    /*
     * Conversational boot (vms-b81): the substrate's boot-flag register is
     * the kernel cmdline (sysboot.h). Reading it needs only the /proc mount
     * just above and produces no output -- checking it here, before
     * anything else in this function runs, is what lets the branch below
     * satisfy docs/design-boot-faithful.md §3.1: NOTHING precedes
     * "SYSBOOT> ", not even the executive-attach line that is otherwise the
     * very first thing this function ever prints.
     */
    if (!sysboot_conversational_requested()) {
        /* ---- Flagless boot: EXACTLY the code that stood here before
         * this item, unchanged in content and order. ---- */

        /* NO hardcoded sethostname() HERE (vms-b6a7: "the hardcoded OVMX
         * hostname DIES"). The real node name is SYS$SYSTEM:OVMXVMSSYS.PAR's
         * SCSNODE parameter, and that file is ON the system disk --
         * unreadable until the disk is mounted and the device table /
         * logical names are up (Step 1b in main(), below).
         * read_boot_parameters() sets the hostname once that is true; see
         * its header for the ordering divergence this forces from real
         * VMS's boot-driver-I/O SYSBOOT read.
         *
         * The executive comes up before anything else runs. */
        executive_attach();

        /* BANNER-FIRST (vms-1fb): SYSBOOT has now handed over -- the
         * executive is attached, and this is the earliest point in the
         * flagless path that guarantees the boot is really coming up (see
         * display_boot_banner()'s header). Everything below this line is
         * system-disk-mount narration and STDRV/STARTUP.COM output, which
         * the oracle (§3.5) shows strictly AFTER the banner. */
        print_banner_once();

        /* MOUNT (vms-651) needs every disk unit's mount point to already
         * exist before any de-privileged VMS session tries to use one --
         * see the function's own comment for why this can only happen
         * here, as root. */
        provision_disk_mount_points();

        /* vmsfs.ko is the filesystem, not the executive; a failure here
         * surfaces as the mount failure below, which halts honestly. The
         * executive itself is loaded and pinned by executive_attach().
         * OVMX-facility, not STARTUP: VMS never narrates a Linux kernel
         * module load, so this is not dressed as a borrowed VMS message
         * (vms-1fb facility audit, docs/design-boot-faithful.md). */
        if (load_kernel_module("/lib/modules/vmsfs.ko") != 0 && errno != EEXIST) {
            fprintf(stderr, "%%OVMX-W-MODFAIL, failed to load vmsfs.ko: %s\n",
                    strerror(errno));
        }

        struct stat vms_st;
        if (stat(SYSDISK_MOUNT, &vms_st) != 0)
            return;  /* No system disk mount point in initramfs */

        /* The system disk must be a real virtio block device. There is no
         * overlay and no auto-initialize fallback: if it is not here, the
         * system does not come up (design-init-scope.md §1). */
        struct stat vda_st;
        if (stat(SYSDISK_DEV, &vda_st) != 0 || !S_ISBLK(vda_st.st_mode)) {
            ovmx_sysinit_halt(
                "no system disk " SYSDISK_DEV " (DKA0:)",
                "the system disk is not present; OVMX does not install one at boot");
        }

        printf("%%OVMX-I-SYSDISK, mounting system disk DKA0:\n");

        /* Mount the pre-installed disk, or halt. A blank or unformatted disk
         * fails to mount as vmsfs -- and PID 1 does NOT initialize it (that
         * is the installer spine's INITIALIZE/PCSI job, run out of band). */
        if (mount(SYSDISK_DEV, SYSDISK_MOUNT, "vmsfs", 0, NULL) != 0) {
            ovmx_sysinit_halt(
                "system disk DKA0: (" SYSDISK_DEV ") would not mount",
                "the volume is not an installed VMSFS system disk; "
                "OVMX does not initialize or install it at boot");
        }

        printf("%%OVMX-I-MOUNTED, system disk DKA0: mounted\n");
        return;
    }

    /*
     * ---- Conversational boot path (vms-b81) ----
     *
     * SYSBOOT needs the system disk mounted to read/write the real
     * SYS$SYSTEM:OVMXVMSSYS.PAR (sysboot.h's header comment explains why it
     * cannot go through the normal filespec translator this early). Mounting
     * it needs only vmsfs.ko, not the executive -- unlike the flagless
     * branch above, executive_attach() is deliberately NOT called until
     * after the prompt returns.
     *
     * The diagnostic lines the flagless branch prints INLINE, as each step
     * happens, are DEFERRED here to after the (optional) prompt instead:
     * same text, same order relative to each other, just moved as a whole
     * block -- because on this path they would otherwise print before
     * SYSBOOT>, which the oracle capture (§3.1) shows nothing does. The
     * banner (vms-1fb) is deferred the same way and for the same reason:
     * §3.1 shows NOTHING preceding "SYSBOOT> ", not even the banner.
     */
    int vmsfs_load_failed =
        (load_kernel_module("/lib/modules/vmsfs.ko") != 0 && errno != EEXIST);
    int vmsfs_errno = errno;

    struct stat vms_st;
    if (stat(SYSDISK_MOUNT, &vms_st) == 0) {
        struct stat vda_st;
        if (stat(SYSDISK_DEV, &vda_st) != 0 || !S_ISBLK(vda_st.st_mode)) {
            ovmx_sysinit_halt(
                "no system disk " SYSDISK_DEV " (DKA0:)",
                "the system disk is not present; OVMX does not install one at boot");
        }
        if (mount(SYSDISK_DEV, SYSDISK_MOUNT, "vmsfs", 0, NULL) != 0) {
            ovmx_sysinit_halt(
                "system disk DKA0: (" SYSDISK_DEV ") would not mount",
                "the volume is not an installed VMSFS system disk; "
                "OVMX does not initialize or install it at boot");
        }

        /* The disk is mounted and SYS$SYSTEM (VMS_SYSTEM_DIR, a raw Linux
         * path -- ovmx_layout.h) is reachable. Load the real parameter file
         * (or factory defaults if it is not there yet), run the prompt, and
         * stash the result for read_boot_parameters(). */
        sysboot_load_working_set(&conversational_boot_params, VMS_SYSTEM_DIR,
                                  "OVMXVMSSYS", "PAR");
        sysboot_run_prompt(&conversational_boot_params, VMS_SYSTEM_DIR,
                            "OVMXVMSSYS", "PAR", VMS_STARTUP_PATH);
        conversational_boot_result_valid = 1;

        /* SYSBOOT (the prompt) has now handed over -- attach the executive
         * and show the banner BEFORE the mount-narration messages below,
         * mirroring the flagless branch's ordering (vms-1fb). */
        executive_attach();
        print_banner_once();

        printf("%%OVMX-I-SYSDISK, mounting system disk DKA0:\n");
        printf("%%OVMX-I-MOUNTED, system disk DKA0: mounted\n");
    }

    if (vmsfs_load_failed)
        fprintf(stderr, "%%OVMX-W-MODFAIL, failed to load vmsfs.ko: %s\n",
                strerror(vmsfs_errno));

    /* Idempotent fallbacks for the degenerate case where SYSDISK_MOUNT did
     * not even exist above (the if-block never ran, so neither call site
     * inside it did either): both functions no-op harmlessly if already
     * done, so this is not a second attach or a second banner on the
     * normal path. */
    executive_attach();
    print_banner_once();
    provision_disk_mount_points();
}

/* ------------------------------------------------------------------ */
/* Installed-volume gate                                              */
/* ------------------------------------------------------------------ */

/*
 * The mounted system disk must be a PROPERLY INSTALLED system volume, or the
 * system does not come up. VMS "finds them or does not boot"
 * (design-init-scope.md §1): the boot chain never creates the system tree, so
 * a volume that mounts but carries no SYS$SYSTEM:DCL.EXE is not one OVMX can
 * boot from -- and PID 1 does NOT install it (that is the installer spine's
 * job). DCL.EXE is the marker: it is the image LOGINOUT activates for every
 * interactive session, so its absence means there is nothing to hand a login
 * to. This replaces is_system_installed()'s "install if missing" branch with
 * an honest halt, exactly as the operator ruled.
 */
static void require_installed_system(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/DCL.EXE", sysexe_linux);
    struct stat st;
    if (stat(path, &st) != 0) {
        ovmx_sysinit_halt(
            "system disk DKA0: is not an installed OVMX system volume",
            "SYS$SYSTEM:DCL.EXE is absent; install the system with the "
            "OVMX installer before booting -- PID 1 does not install one");
    }
}

/* ------------------------------------------------------------------ */
/* Boot parameters (vms-b6a7)                                         */
/* ------------------------------------------------------------------ */

/*
 * read_boot_parameters - SYSBOOT's job: read SYS$SYSTEM:OVMXVMSSYS.PAR and
 * set the system's identity from it. Called once the system disk is mounted
 * AND confirmed installed (require_installed_system(), above), and once the
 * device table / logical names are live (main()'s Step 1b) -- vmsfs
 * translation of VMS_PARAMS_PATH needs both.
 *
 * ORDERING DIVERGENCE, DISCLOSED (docs/design-boot-faithful.md, "ORDERING
 * NOTE"): real VMS's SYSBOOT reads the parameter file BEFORE the executive
 * attaches, through boot-driver I/O that never mounts a filesystem in the
 * Unix sense. OVMX's substrate has no analogue of boot-driver I/O -- the
 * parameter file is an ordinary file ON the system disk, unreadable until
 * vmsfs has mounted it -- so this necessarily runs AFTER the mount and
 * AFTER the executive attaches. Named here as a substrate limit, not hidden
 * as if OVMX read it VMS's way (the same discipline provision_ownership()'s
 * MAXSYSGROUP note already carries).
 *
 * SHARES THE READER (vms-9b7): sysgen_read_string()/sysgen_read_param() are
 * the exact functions vms_sysgen.c, scsd.c and the DCL F$GETSYI lexicals
 * already use. This function adds no parser of its own -- it links vmsfs
 * (statically, via the existing vmsfs_static target) and calls the one
 * reader every other consumer calls. "Parameters the executive or later
 * stages need are held for them" is satisfied by the parameter file
 * REMAINING on the mounted, persistent system disk: PROVISION.EXE and every
 * later stage can read the identical file through the identical reader --
 * no separate in-memory hand-off is needed or added.
 *
 * Missing or corrupt SYS$SYSTEM:OVMXVMSSYS.PAR on an already-INSTALLED
 * volume (require_installed_system() already confirmed DCL.EXE is present)
 * is NOT the mount-or-halt condition above. There is no oracle capture of
 * what VMS does with a missing/corrupt ALPHAVMSSYS.PAR, so OVMX does not
 * invent a VMS message for it (Rule 10): it boots on the compiled-in
 * default node name and says so with an explicitly OVMX-facility warning --
 * loud, not silent, and never dressed up as a VMS diagnostic.
 */
static void read_boot_parameters(void)
{
    /*
     * A SYSBOOT> conversational session already loaded (and possibly SET)
     * the parameter set this boot, in bare_metal_init() -- vms-b81. Use
     * THAT in-memory result instead of re-reading OVMXVMSSYS.PAR from disk:
     * re-reading would silently discard an in-memory-only SET (VMS
     * persistence semantics -- SET is this-boot-only, WRITE is the only
     * thing that touches the file, and CONTINUE does not call WRITE). A
     * flagless boot never sets conversational_boot_result_valid and falls
     * through to the original read below, unchanged.
     */
    if (conversational_boot_result_valid) {
        const char *node = sysboot_get_string(&conversational_boot_params, "SCSNODE");
        if (node && node[0] != '\0') {
            sethostname(node, strlen(node));
            printf("%%OVMX-I-SCSNODE, node name %s set from SYS$SYSTEM:OVMXVMSSYS.PAR\n",
                   node);
            return;
        }
        fprintf(stderr,
                "%%OVMX-W-NOPARAMS, SYS$SYSTEM:OVMXVMSSYS.PAR is missing or unreadable\n");
        fprintf(stderr,
                "-OVMX-I-NOPARAMS, booting with default node name %s\n",
                OVMX_DEFAULT_NODENAME);
        sethostname(OVMX_DEFAULT_NODENAME, strlen(OVMX_DEFAULT_NODENAME));
        return;
    }

    char node[SYSGEN_STRVAL_LEN];

    if (sysgen_read_string("SCSNODE", node, sizeof(node)) == 0 && node[0] != '\0') {
        sethostname(node, strlen(node));
        printf("%%OVMX-I-SCSNODE, node name %s set from SYS$SYSTEM:OVMXVMSSYS.PAR\n",
               node);
        return;
    }

    fprintf(stderr,
            "%%OVMX-W-NOPARAMS, SYS$SYSTEM:OVMXVMSSYS.PAR is missing or unreadable\n");
    fprintf(stderr,
            "-OVMX-I-NOPARAMS, booting with default node name %s\n",
            OVMX_DEFAULT_NODENAME);
    sethostname(OVMX_DEFAULT_NODENAME, strlen(OVMX_DEFAULT_NODENAME));
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

/*
 * %STDRV-I-STARTUP, begun -- printed by STDRV, the startup driver, BEFORE the
 * startup procedure runs (F1, docs/design-init-scope.md §3.1). On VMS this
 * line BRACKETS the startup: "begun" precedes every phase. It used to print
 * from display_boot_banner() AFTER run_startup() had already finished -- an
 * empty bracket, re-reading the clock between two printfs with no work between
 * them -- so it is moved here and called immediately before run_startup().
 *
 * There is NO "completed" counterpart. The Alpha 8.4 clean-boot capture
 * (2026-08-07, docs/design-boot-faithful.md §3.5) shows VMS prints
 * "%STDRV-I-STARTUP, OpenVMS startup begun" ONCE and no "completed" line
 * anywhere in the boot; the old "completed" line was an OVMX invention and is
 * deleted, not repositioned.
 */
static void print_stdrv_begun(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    printf("%%STDRV-I-STARTUP, " OVMX_PRODUCT_NAME " startup begun at "
           "%2d-%s-%04d %02d:%02d:%02d.%02d\n",
           tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));
    fflush(stdout);
}

/*
 * The OS identification banner (product name/version + copyright block).
 *
 * BANNER-FIRST (vms-1fb, docs/design-boot-faithful.md §2.5/§3.5/§4 item 3).
 * The Alpha oracle capture (§3.5) shows this banner printing IMMEDIATELY
 * after SYSBOOT hands over -- before %STDRV-I-STARTUP, before the
 * site-specific-startup announcement, before anything STARTUP.COM prints.
 * It used to print from here at the very end of main(), after run_startup()
 * had already run STARTUP.COM to completion -- last, not first, exactly
 * backwards from the oracle. It is now called through print_banner_once()
 * (below) from the point in the boot that corresponds to SYSBOOT's handoff:
 * right after executive_attach() succeeds, in bare_metal_init(), BEFORE the
 * system-disk mount messages, the SCSNODE identity line, %STDRV-I-STARTUP,
 * or any STARTUP.COM output.
 *
 * WHY "right after executive_attach() succeeds" and not literally first:
 * OVMX's executive-is-integral guarantee (executive_attach()'s own header
 * comment; tests/qemu/test_executive_integral.sh) requires that the banner
 * NEVER appear on a boot that does not come up because the executive itself
 * would not attach -- a banner is an implicit "the system is here" signal,
 * and printing one ahead of a guaranteed-fatal executive failure would be
 * exactly the false-success LARP the authenticity rules forbid. Gating the
 * banner on a successful executive_attach() preserves that guarantee while
 * still satisfying "before any mount message or STARTUP output": SYSBOOT's
 * own job (load the executive) has already completed by the time CONTINUE
 * is issued in the oracle capture too, so this ordering is not a compromise
 * of the oracle shape, it is what makes OVMX's EXEC_INIT-equivalent visible
 * at the same point VMS's own boot narrative implies it already happened.
 *
 * A boot can still halt AFTER this (system-disk mount failure, SYSINIT-
 * equivalent) with the banner already shown -- see
 * tests/qemu/test_persistent_boot.sh's negative control, which documents
 * why that is honest rather than a regression.
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
}

/*
 * print_banner_once - display_boot_banner(), exactly once, no matter how
 * many of the call sites below actually run for a given boot path (the
 * flagless and conversational branches of bare_metal_init(), and the
 * non-bare-metal fallback in main()). Idempotent so every call site can
 * call it unconditionally at "SYSBOOT just handed over" without each one
 * having to know whether an earlier call site already fired.
 */
static int banner_printed = 0;

static void print_banner_once(void)
{
    if (banner_printed)
        return;
    banner_printed = 1;
    display_boot_banner();
}

/*
 * Main: the boot sequence, then wait. See the "IT DOES NOT RUN THE CONSOLE
 * LOGIN LOOP" note in this file's header (vms-8d2) for where that moved.
 */
int main(void)
{
    /*
     * Substrate identity line -- printed FIRST, before any mount, module
     * load, or VMS-flavored boot message. GNU/Linux-style dual identity
     * (operator ruling, rd vms-700/vms-296/vms-3de): "OVMX/Linux" names
     * the Linux-kernel substrate this boots on, distinct from and prior to
     * the "OpenVMX" product identity STARTUP.EXE hands off to later
     * (display_boot_banner(), below) -- the boot reads "OVMX/Linux"
     * substrate coming up, THEN "OpenVMX" product. Unconditional (not
     * gated on getpid() == 1): cheap, and every invocation of this binary
     * is still OVMX/Linux underneath regardless of PID.
     */
    printf("%s\n", ovmx_substrate_banner());
    fflush(stdout);

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

    /* Same idempotent fallback as bare_metal_init()'s own trailing call
     * (vms-1fb): on a substrate that skipped bare_metal_init() entirely
     * (getpid() != 1, or /proc already mounted) the banner has not printed
     * yet, and this is the earliest point that still guarantees the
     * executive is really up. A no-op on the normal PID 1 bare-metal boot,
     * where bare_metal_init() already showed it. */
    print_banner_once();

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

    /* Step 2: The system disk must already carry an installed system, or the
     * boot stops here. PID 1 NO LONGER INSTALLS, INITIALIZES, SEEDS OR
     * PROVISIONS anything (operator ruling 2026-08-10, "STRIP ALL OF IT",
     * docs/design-init-scope.md §2/§6): install_system(), provision_dirs(),
     * provision_seed_files(), the INITIALIZE-on-blank fork, the overlay mode,
     * and the INITRAMFS_BACKUP copy dance are all deleted. Installation lives
     * ENTIRELY in the installer spine (vms-791 -> vms-8ab mastered disk image
     * -> vms-df9 PCSI). A booting VMS system finds its system disk installed or
     * does not boot -- so a mounted volume without SYS$SYSTEM:DCL.EXE is a
     * fail-honest halt, never a self-install. */
    require_installed_system();

    /* Step 2b: SYSBOOT's job -- read SYS$SYSTEM:OVMXVMSSYS.PAR and set the
     * system's identity (sethostname) from its SCSNODE parameter (vms-b6a7,
     * docs/design-boot-faithful.md §4.1). Must run after Step 1b (the
     * device table / logical names that make VMS_PARAMS_PATH resolvable)
     * and after require_installed_system() confirms this is a real
     * installed volume, not a blank one. See read_boot_parameters()'s own
     * header for the ordering divergence from real VMS and the honest
     * missing/corrupt-file fallback. */
    read_boot_parameters();

    /* Step 3: %STDRV-I-STARTUP begun, then run STARTUP.COM.
     * The STDRV "begun" bracket precedes the startup procedure (F1). There is
     * NO "completed" counterpart anywhere in the boot (§3.5 oracle; the old
     * invented completed line stays deleted, vms-2f0/vms-1fb).
     *
     * There is no logical name daemon to start first: on VMS the logical name
     * tables are executive-resident, not a service (vms-a4b).
     *
     * run_startup() is ALSO where the system's services start, and the only
     * place they do: STARTUP.COM invokes SYSTARTUP_VMS.COM, which invokes each
     * service's own startup procedure, which creates it with RUN/DETACHED under
     * a VMS process name (vms-47b). STARTUP.EXE deliberately starts none of its
     * own -- see the NOTE ON SERVICES above. */
    print_stdrv_begun();
    run_startup();

    /* Step 4: the boot banner already printed (vms-1fb) -- back in
     * bare_metal_init()/the executive gate above, right after SYSBOOT handed
     * over, well before this point. This call is the same idempotent
     * fallback as those: a no-op on every real boot path, and the only thing
     * that would ever make it fire here is a substrate this file does not
     * otherwise anticipate. Kept for the same reason the other two fallback
     * call sites are: never reach STDRV/STARTUP.COM output without the
     * banner already shown, on ANY path. */
    print_banner_once();
    fflush(stdout);
    fflush(stderr);

    /* Step 5: PID 1's job ends here.
     *
     * The console login loop used to run inline below, forking LOGINOUT.EXE
     * on the console forever -- the exact "wrong component" defect
     * design-init-scope.md §2 named it (JOB_CONTROL's job, run from
     * STARTUP.EXE). run_startup() above already ran STARTUP.COM to
     * completion, which -- through SYSTARTUP_VMS.COM and SYS$STARTUP:
     * JOB_CONTROL_STARTUP.COM -- has already created JOB_CONTROL as a
     * DETACHED process holding the console (vms-8d2, vms-47b's RUN/DETACHED
     * mechanism); by the time control reaches this line, the console session
     * is JOB_CONTROL's, not PID 1's, and grep for a login loop here finds
     * none.
     *
     * PID 1 is Linux's process 1: it cannot exit without panicking the
     * kernel, and there is no VMS analogue of that requirement to satisfy
     * instead (VMS's SYSINIT/STARTUP process has no such constraint --
     * docs/design-boot-faithful.md §3.6's fresh-boot process population
     * does not even list it as still running). So it waits, doing the one
     * thing Linux still requires of PID 1: answer a shutdown signal and
     * reap anything reparented to it (the SIGCHLD handler installed above
     * already does the reaping; JOB_CONTROL and everything it creates are
     * not PID 1's children and are never waited on here -- see the comment
     * this replaced, and the NOTE ON SERVICES above). */
    while (!shutdown_requested) {
        pause();
    }

    return 0;
}
