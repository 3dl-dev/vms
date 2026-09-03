/*
 * ovmx_init.c - OVMX Boot Orchestrator (STARTUP.EXE)
 *
 * PID 1 / ENTRYPOINT for OVMX on its one runtime target: the real-kernel /
 * QEMU path (CLAUDE.md Rule 9). This is SYSBOOT + EXEC_INIT + SYSINIT and
 * NOTHING ELSE (docs/design-init-scope.md, operator ruling 2026-08-10 "STRIP
 * ALL OF IT"): mount the Linux base layer, attach the executive, mount the
 * system disk through the executive Files-11 ACP -- or halt -- hand off to
 * STARTUP.COM, then wait.
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
 * JOB_CONTROL, a DETACHED process STARTUP.COM's phase driver creates --
 * never by the SYSINIT-equivalent bootstrap process. That loop now lives in
 * src/ovmx_job_control/ovmx_job_control.c (JOB_CONTROL.EXE), created by
 * SYS$STARTUP:JOB_CONTROL_STARTUP.COM (registered as an END-phase STDRV
 * component in SYS$STARTUP:VMS$VMS.DAT and run from STARTUP.COM's own
 * RUN_COMPONENTS subroutine, vms-2a9 -- run_startup() below just runs
 * STARTUP.COM, which runs the phase driver) with RUN/DETACHED/PROCESS_NAME=
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
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>

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
/* CLUSTER_AUTHORIZE.DAT reader (vms-ci.8) -- group + password, loaded into
 * the executive alongside the SYSGEN cluster parameters below (FC-P0.10). */
#include "cluster_authorize.h"
/* The FC-P0.11 VAXCLUSTER gate: the ONE function that decides whether the
 * boot path issues VMS_IOCTL_CLUSTER_START, shared with its R1 host test
 * (tests/cluster/host/test_cluster_boot_gate.c) so neither copy can drift. */
#include "cluster_boot_gate.h"
/* SYSBOOT> conversational-boot prompt (vms-b81) -- operates on the SAME
 * SYS$SYSTEM:OVMXVMSSYS.PAR every other consumer uses. On the Linux atomic-flip
 * runtime (vms-46c) it reaches that file over the executive Files-11 ACP, the
 * same genuine ODS-2 volume the flagless path mounts -- NO /vms passthrough. The
 * NetBSD-vax backend still resolves it to a raw Linux directory (VMS_SYSTEM_DIR)
 * until its own flip (vms-d5d). Either way SYSBOOT runs before the device table
 * exists, which is why it takes the physical path, not vmsfs_to_linux_path() --
 * see sysboot.h and bare_metal_init() below. */
#include "sysboot.h"
/* Boot-plumbing substrate seam (vms-28f, epic vms-8e8): the ONE header PID 1
 * includes for the host-OS boot primitives it needs -- load-executive-module,
 * mount kernel filesystems + the system disk, resolve/name the boot block
 * device, open the executive device, start the /dev/kmsg operator-console
 * bridge (vms-32a), and power off. Every raw Linux boot syscall this file used
 * to make inline now lives behind an ovmx_boot_* op, so ONE ovmx_init.c serves
 * every substrate with no #ifdef fork (INV-DRIFT). The Linux backend
 * (ovmx_boot_linux.c) maps each op to the exact syscall used before the seam;
 * the NetBSD backend is vms-f2e. See ovmx_boot.h and docs/design-p4-netbsd-vax-
 * boot.md (GAP-C). */
#include "ovmx_boot.h"
#if defined(OVMX_BOOT_ACP_BRIDGE)
/* ACP-read bootstrap bridge (vms-5f0): PID 1 stages the first-hop execve'd
 * images (IMGACT/PROVISION/DCL/JOB_CONTROL/LOGINOUT) off the genuine ODS-2
 * boot volume THROUGH the executive ACP into OVMX_BOOT_STAGE_DIR, so the host
 * kernel can execve them with the /vms POSIX passthrough retired.
 *
 * OVMX_BOOT_ACP_BRIDGE means exactly one thing: "the ACP-read bridge
 * translation units (ovmx_boot_acp_read.c + ovmx_boot_sysgen_acp.c +
 * imgact_acp.c) are linked into THIS PID 1". It is defined by whichever build
 * recipe puts them in the link -- src/ovmx_init/CMakeLists.txt's Linux branch
 * (which sets OVMX_BOOT_BRIDGE_SRC), and the elf32-vax compile audit
 * tools/cross-vax/build-acp-read-audit-vax.sh. It is deliberately NOT
 * OVMX_BOOT_LINUX (which names the SUBSTRATE, not the link set): the bridge is
 * substrate-neutral C, and the NetBSD-vax runtime cutover (vms-329) turns it on
 * by adding these TUs + this macro to the VAX recipe -- a wiring change, with
 * the compile already proven by vms-8e8f. Until then the VAX runtime does not
 * define it and its boot behaviour is unchanged. */
#include "ovmx_boot_acp_read.h"
#endif

/*
 * SYS$SYSTEM as a Linux path — initialized at runtime after the device table
 * is populated so VMS specs can be translated. Used only to confirm the mounted
 * system disk is a properly installed volume (DCL.EXE present) before handing
 * off to STARTUP.COM.
 */
static char sysexe_linux[512];
static char syslib_linux[512];

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
        ovmx_boot_power_off();
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
 * (vms_kif_devscan(), src/kernel/vms_devtab.c -- vda -> VDA0:, vdb ->
 * VDA100:, ...), not a second, independent scan of /dev (Rule 11): PID 1
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

/* When set before executive_attach(), the executive is attached SILENTLY: the
 * %OVMX-I-EXEC console line is suppressed and executive_announce() emits it
 * later. The conversational boot path (vms-46c) must attach the executive BEFORE
 * the SYSBOOT> prompt -- the Files-11 ACP $MOUNT of the system disk needs it --
 * but must emit NOTHING before "SYSBOOT> " (docs/design-boot-faithful.md §3.1),
 * so it sets this, calls executive_attach(), and announces after the prompt.
 * Every other path leaves it 0 and gets the announce inline from
 * executive_attach(). It is a deferral of the ANNOUNCE only -- the executive
 * guarantee (capture + halt-on-failure + pin) is unconditional either way. */
static int executive_announce_deferred = 0;

/* Emit the %OVMX-I-EXEC line for an executive executive_attach() opened while
 * executive_announce_deferred was set (the conversational path, after SYSBOOT>). */
static void executive_announce(void)
{
    printf("%%OVMX-I-EXEC, VMS executive attached on /dev/vms\n");
}

/*
 * executive_attach - THE boot-time executive guarantee (Rule 9 / INV-6). PID 1
 * loads vms.ko, opens /dev/vms through the boot seam, CAPTURES the descriptor,
 * and HALTS if it cannot: OVMX refuses to run without its executive. executive_fd
 * (file-static) is then pinned for the life of the system and never closed here,
 * so vms.ko cannot be rmmod'd out from under a running OVMX.
 *
 * The guarantee lives in executive_attach() ITSELF, not in a helper it delegates
 * to: tests/integration/test_runtime_target.sh inspects the body of the function
 * literally named executive_attach() for the capture, the terminal-halt failure
 * branch, and the pin. (vms-46c had split the capture+halt into a separate
 * executive_attach_silent() to serve the conversational boot path, which moved
 * the guarantee out of the function the gate reads and reddened it; the announce
 * is deferred here with executive_announce_deferred instead, keeping the whole
 * guarantee -- and the %OVMX-I-EXEC line the gate's close-pin control mutates --
 * inside executive_attach().)
 *
 * Idempotent. With executive_announce_deferred set the attach is SILENT (the
 * conversational path prints the %OVMX-I-EXEC line later via executive_announce());
 * otherwise it is emitted inline here.
 */
static void executive_attach(void)
{
    if (executive_fd >= 0)
        return;                 /* already attached; idempotent by design */

    if (ovmx_boot_load_module("vms") != 0 && errno != EEXIST) {
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

    executive_fd = ovmx_boot_open_executive();
    if (executive_fd < 0) {
        /* No oracle analogue: a VMS executive has no device node to open, so
         * VMS is never in this state and prints no status for it. This is an
         * OVMX event, not a VMS one -- it must not wear the EXECINIT
         * facility (Rule 10). */
        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));
    }

    if (executive_announce_deferred)
        return;                 /* conversational path: %OVMX-I-EXEC deferred to
                                 * executive_announce() after the SYSBOOT> prompt */
    printf("%%OVMX-I-EXEC, VMS executive attached on /dev/vms\n");
}

/*
 * report_kernel_taint() - OVMX taint-audit readout (rd vms-566, epic vms-19e
 * "owns-kernel").
 *
 * After the OVMX executive module has loaded -- vms.ko via executive_attach()
 * (vms-165 retired the separate vmsfs.ko VFS module; the ODS-2 ACP is in vms.ko
 * now) -- read the REAL
 * /proc/sys/kernel/tainted mask and, IF the boot-flag register (kernel cmdline)
 * carries the token "ovmx.taintreport", print it as an OVMX-facility line the
 * taint-clean acceptance gate scrapes (tests/qemu/test_kernel_taint.sh). This is
 * the GROUND-SOURCE read that gate's numeric bit test keys on (Rule 7): the
 * actual booted kernel's taint state, after the actual modules loaded -- never a
 * stubbed value. The two bits the untaint work forbids are named inline:
 * O = out-of-tree module (bit 12 = 4096, cleared by vms-934's in-tree build) and
 * E = unsigned module (bit 13 = 8192, cleared by vms-ff5's module signing).
 *
 * Gated behind the cmdline flag ON PURPOSE. The normal (faithful) boot console
 * is pinned to an oracle-derived facility sequence (tests/qemu/
 * test_boot_conformance.sh and test_job_control_console.sh) and must stay
 * pristine, so this diagnostic line appears ONLY when a taint audit explicitly
 * asks for it -- the flag is off on every normal boot. The mask VALUE is real
 * either way; the flag controls the diagnostic PRINT, not the read. Reading a
 * boot-flag token here (rather than always printing) is exactly how the boot
 * seam keeps substrate diagnostics out of the pinned sequence.
 *
 * OVMX-facility (Rule 10): VMS never reports a Linux kernel taint mask, so this
 * is not dressed as a borrowed VMS message -- same class as the %OVMX-W-MODFAIL
 * / %OVMX-I-EXEC lines this file already prints for Linux-substrate boot steps.
 */
static void report_kernel_taint(void)
{
    /* Boot-flag register (kernel cmdline): only emit when the audit asked. */
    FILE *cf = fopen("/proc/cmdline", "r");
    if (!cf)
        return;
    char cmd[1024];
    size_t clen = fread(cmd, 1, sizeof(cmd) - 1, cf);
    fclose(cf);
    cmd[clen] = '\0';
    if (!strstr(cmd, "ovmx.taintreport"))
        return;

    FILE *tf = fopen("/proc/sys/kernel/tainted", "r");
    if (!tf) {
        printf("%%OVMX-W-TAINT, /proc/sys/kernel/tainted unreadable: %s\n",
               strerror(errno));
        return;
    }
    /* fread + strtoul, not fscanf -- the same primitives sysboot.c uses to read
     * the boot-flag register, so this stays within the ovmx_init libc subset on
     * every substrate (Linux and the NetBSD/VAX SYSKRNL). */
    char tbuf[64];
    size_t tlen = fread(tbuf, 1, sizeof(tbuf) - 1, tf);
    fclose(tf);
    tbuf[tlen] = '\0';
    char *end = tbuf;
    unsigned long mask = strtoul(tbuf, &end, 10);
    if (end == tbuf) {
        printf("%%OVMX-W-TAINT, /proc/sys/kernel/tainted did not parse\n");
        return;
    }

    /* The gate keys on the numeric mask; the O:/E: text is for the human
     * reading the console, and names exactly the two forbidden bits. */
    printf("%%OVMX-I-TAINT, kernel taint mask = %lu (0x%lx)%s%s\n",
           mask, mask,
           (mask & 4096UL) ? " O:out-of-tree" : "",
           (mask & 8192UL) ? " E:unsigned" : "");
}

/*
 * Bare-metal bootstrap: mount the Linux base layer, set hostname, attach the
 * executive, and MOUNT THE SYSTEM DISK (through the executive Files-11 ACP) OR
 * HALT. Called when running as PID 1 on bare metal or QEMU.
 *
 * This is SYSINIT: it mounts the pre-installed system disk. It does NOT
 * install, initialize, or fall back to an ephemeral overlay -- a booting VMS
 * system finds its system disk or does not boot (design-init-scope.md §1,
 * operator ruling 2026-08-10). A disk that is absent or will not mount is a
 * fail-honest halt (ovmx_sysinit_halt), never a silent overlay onto a tmpfs
 * tree the way the deleted code did.
 */
/*
 * BOOT-CONSOLE ECHO SILENCE (vms-dec).
 *
 * The operator console (OPA0: -> /dev/console) is a cooked-mode tty with ECHO
 * on. During the slow boot no login prompt is visible yet, so an operator
 * naturally strikes RETURN several times -- and the tty echoes each keystroke
 * at type-time as a BLANK LINE, scattered through the boot narration: the
 * operator-reported "newline spam". PID 1 owns the console for the WHOLE boot
 * -- LOGINOUT is not forked until STARTUP.COM's tail (JOB_CONTROL, seen as the
 * final %RUN-S-PROC_ID line) -- so the echo has to be silenced HERE, not in
 * LOGINOUT, to cover the entire window the operator is hammering. Silencing it
 * only inside LOGINOUT's wake catches almost none of the mashing, because the
 * mashing happens long before LOGINOUT exists.
 *
 * ECHO gates INPUT echo only: the boot's own OUTPUT is untouched. LOGINOUT
 * RE-ENABLES ECHO right before it prints "Username:" (console_login() in
 * tools/vms_login.c), so the operator's typed username shows normally. termios
 * is per-tty device state shared across the exec/fork chain that reaches the
 * same /dev/console, so clearing it here persists until LOGINOUT restores it.
 *
 * OVMX console-handling behaviour (CLAUDE.md Rule 8), not a claimed VMS
 * terminal-driver byte detail. Best-effort: never blocks or fails the boot.
 * Deliberately NOT applied across the conversational SYSBOOT> prompt, which
 * must keep echoing the commands the operator types there; it is applied on
 * that path only after the prompt hands over, before the boot narration.
 */
static void boot_console_disable_echo(void)
{
    int fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        return;
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        t.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(fd, TCSANOW, &t);
    }
    close(fd);
}

static void bare_metal_init(void)
{
    /* Mount essential filesystems (proc/sys/dev/tmp/pts/shm) through the
     * boot seam, which runs the identical mount sequence for this substrate. */
    ovmx_boot_mount_kernel_filesystems();

    /*
     * The kernel-log -> operator-console bridge (vms-32a) starts as early as
     * /dev exists, ahead of BOTH boot branches below, so it is running
     * before vms.ko loads in either one and replays its init-time
     * records rather than missing them. See docs/design-opcom-executive-
     * logging.md. Best-effort: the bridge never blocks or fails boot even if
     * the substrate's kernel-log source is unavailable.
     */
    ovmx_boot_start_console_log_bridge();

    /*
     * Lower the kernel's OWN console log level (vms-300) BEFORE either boot
     * branch below makes its first ovmx_boot_load_module() call -- the
     * earliest point in the boot path before any kernel module is loaded.
     * The bridge just started above keeps reading every kmsg record for
     * OPERATOR.LOG regardless of this; only the console SINK is muted. See
     * ovmx_boot_mute_kernel_console()'s header comment (ovmx_boot.h).
     */
    ovmx_boot_mute_kernel_console();

    /*
     * Conversational boot (vms-b81): the platform's boot-flag register is
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

        /* Silence the console's echo of operator RETURN keystrokes for the
         * duration of the boot (vms-dec) -- before the first narration line
         * below, so the whole hammering window is covered. LOGINOUT re-enables
         * ECHO at its "Username:" prompt. */
        boot_console_disable_echo();

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

        /* The system disk must be a real block device. There is no overlay
         * and no auto-initialize fallback: if it is not here, the system does
         * not come up (design-init-scope.md §1). */
        if (!ovmx_boot_system_disk_present()) {
            char msg[128];
            snprintf(msg, sizeof(msg), "no system disk %s (%s)",
                     ovmx_boot_system_disk_dev(), ovmx_boot_system_disk_unit());
            ovmx_sysinit_halt(
                msg,
                "the system disk is not present; OVMX does not install one at boot");
        }

        printf("%%OVMX-I-SYSDISK, mounting system disk %s\n",
               ovmx_boot_system_disk_unit());

        /* Mount the system disk via the substrate's own mechanism. The boot
         * seam keeps ovmx_init.c substrate-neutral -- ONE source, no #ifdef
         * (INV-DRIFT); the substrate split lives ONLY in ovmx_boot_linux.c /
         * ovmx_boot_netbsd.c:
         *   Linux  -- ATOMIC FLIP (vms-5f0, epic vms-208): $MOUNT the boot unit
         *             through the Files-11 (ODS-2) ACP in the executive, NOT a
         *             VFS mount of a bespoke-VMFS volume at /vms.
         *             SYS$DISK is now a genuine ODS-2 block device the ACP owns;
         *             every consumer (RMS/DCL/IMGACT/LOGINOUT) reaches it by
         *             $ASSIGN + $QIO. The executive is already attached
         *             (executive_attach() above), which the ACP $MOUNT requires.
         *   NetBSD -- the SAME executive Files-11 ACP in the vms module
         *             (vms-329 VAX ACP cutover; vms-165 retired the separate
         *             vmsfs VFS module on both substrates).
         * Either way a blank/unformatted or non-installed volume fails to mount
         * and PID 1 halts here -- it does NOT initialize it (the installer
         * spine's INITIALIZE/PCSI job runs out of band). */
        if (ovmx_boot_mount_system_disk_native() != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "system disk %s (%s) would not mount",
                     ovmx_boot_system_disk_unit(), ovmx_boot_system_disk_dev());
            ovmx_sysinit_halt(
                msg,
                "the volume is not an installed genuine system disk; "
                "OVMX does not initialize or install it at boot");
        }

        printf("%%OVMX-I-MOUNTED, system disk %s mounted\n",
               ovmx_boot_system_disk_unit());

        /* Publish the DISCOVERED system device (vms-9f5) so every child --
         * imgact.c, lnm_defaults.c, DCL -- resolves SYS$SYSDEVICE to the disk
         * PID 1 actually mounted, not a compile-time guess. This is the boot
         * chain fulfilling the OVMX_SYSDEVICE contract those readers assume. */
        setenv("OVMX_SYSDEVICE", ovmx_boot_system_disk_unit(), 1);

        /* The OVMX executive module is loaded (vms.ko via executive_attach()
         * above; vms-165 retired the separate vmsfs.ko): the taint mask is now
         * final. Emits ONLY under the ovmx.taintreport boot flag (vms-566). */
        report_kernel_taint();
        return;
    }

    /*
     * ---- Conversational boot path (vms-b81) ----
     *
     * SYSBOOT reads/writes the real SYS$SYSTEM:OVMXVMSSYS.PAR (sysboot.h's
     * header explains why it cannot go through the normal filespec translator
     * this early). The diagnostic lines the flagless branch prints INLINE are
     * DEFERRED here to after the (optional) prompt -- same text, same order
     * relative to each other, moved as a whole block -- because on this path
     * they would otherwise print before "SYSBOOT> ", which the oracle capture
     * (§3.1) shows nothing does. The banner (vms-1fb) and the %OVMX-I-EXEC line
     * are deferred the same way and for the same reason.
     */
    /*
     * ATOMIC FLIP (vms-46c): SYSBOOT reaches OVMXVMSSYS.PAR over the executive
     * Files-11 (ODS-2) ACP -- the SAME genuine volume the flagless path mounts,
     * with NO /vms passthrough (that residual is exactly what vms-46c excises).
     * The ACP $MOUNT requires the executive, so it is attached HERE, before the
     * prompt -- but SILENTLY (executive_announce_deferred): §3.1 shows NOTHING
     * precedes "SYSBOOT> ", not even %OVMX-I-EXEC. A disk that is absent or will
     * not ACP-mount is a fail-honest halt, exactly as on the flagless path --
     * OVMX finds its installed system disk or does not boot (design-init-scope.md
     * §1); it never legacy-mounts vmsfs or installs.
     */
    executive_announce_deferred = 1;   /* attach silently; announce after SYSBOOT> */
    executive_attach();

    if (!ovmx_boot_system_disk_present()) {
        char msg[128];
        snprintf(msg, sizeof(msg), "no system disk %s (%s)",
                 ovmx_boot_system_disk_dev(), ovmx_boot_system_disk_unit());
        ovmx_sysinit_halt(
            msg,
            "the system disk is not present; OVMX does not install one at boot");
    }
    if (ovmx_boot_mount_system_disk_native() != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "system disk %s (%s) would not mount",
                 ovmx_boot_system_disk_unit(), ovmx_boot_system_disk_dev());
        ovmx_sysinit_halt(
            msg,
            "the volume is not an installed genuine system disk; "
            "OVMX does not initialize or install it at boot");
    }
    /* Publish the discovered system device (vms-9f5), as on the flagless path. */
    setenv("OVMX_SYSDEVICE", ovmx_boot_system_disk_unit(), 1);

    /* The system disk is ACP-mounted. Load the real parameter set over the ACP
     * (or factory defaults if the volume has none yet), run the prompt, and
     * stash the result for read_boot_parameters(). sysboot's ACP reader/writer
     * is silent -- no output precedes "SYSBOOT> ". */
    sysboot_load_working_set(&conversational_boot_params, VMS_SYSTEM_DIR,
                              "OVMXVMSSYS", "PAR");
    sysboot_run_prompt(&conversational_boot_params, VMS_SYSTEM_DIR,
                        "OVMXVMSSYS", "PAR", VMS_STARTUP_PATH);
    conversational_boot_result_valid = 1;

    /* SYSBOOT> has handed over: from here the boot narration prints and the
     * operator may be hammering RETURN waiting for the login. Silence the
     * console echo now (vms-dec) -- AFTER the prompt above, which had to keep
     * echoing the commands the operator typed into it. LOGINOUT re-enables
     * ECHO at its "Username:" prompt. */
    boot_console_disable_echo();

    /* SYSBOOT has handed over -- emit the deferred narration in the flagless
     * branch's order (vms-1fb): the executive-attach line (already attached
     * above, announced now), the banner, then the mount lines. */
    executive_announce();
    print_banner_once();
    printf("%%OVMX-I-SYSDISK, mounting system disk %s\n",
           ovmx_boot_system_disk_unit());
    printf("%%OVMX-I-MOUNTED, system disk %s mounted\n",
           ovmx_boot_system_disk_unit());

    /* vms.ko is loaded (executive_attach() above, silent); emit the taint mask
     * readout, gated on the ovmx.taintreport boot flag (vms-566). */
    report_kernel_taint();

    provision_disk_mount_points();
    /* vms-329: there is no non-ACP arm here any more. The netbsd-vax #else
     * branch that used to VFS-mount SYS$DISK with vmsfs.ko is GONE: the
     * coupled cutover made the ACP $MOUNT the only mount on every runtime
     * substrate, and NetBSD's spec_vnops allows exactly ONE open of the
     * block device, so a VFS fallback could not coexist with it even if it
     * were wanted. ACP or fail-honest (INV-6). */
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
    /*
     * ATOMIC FLIP (vms-5f0): the marker is probed THROUGH THE EXECUTIVE ACP,
     * not with a POSIX stat() of /vms. SYS$DISK is now a genuine ODS-2 volume
     * the ACP owns ($MOUNTed in bare_metal_init); the /vms passthrough is
     * retired, so DCL.EXE has no POSIX path to stat. ovmx_boot_acp_present()
     * $ASSIGNs a file-class channel + IO$_ACCESSes the file over /dev/vms --
     * the same file access IMGACT uses to activate it. Fail-honest: a missing
     * file or an unreachable executive both read as "not installed", and the
     * halt message is unchanged. NEVER a faked presence (INV-6).
     */
    char path[512];
    char halt[128];
    snprintf(path, sizeof(path), "%s/DCL.EXE", sysexe_linux);
#if defined(OVMX_BOOT_ACP_BRIDGE)
    if (!ovmx_boot_acp_present(path)) {
#else
    /* No ACP-read bridge in this link (the pre-cutover VAX runtime, vms-329):
     * unchanged POSIX presence probe. */
    struct stat st;
    if (stat(path, &st) != 0) {
#endif
        snprintf(halt, sizeof(halt),
                 "system disk %s is not an installed OVMX system volume",
                 ovmx_boot_system_disk_unit());
        ovmx_sysinit_halt(
            halt,
            "SYS$SYSTEM:DCL.EXE is absent; install the system with the "
            "OVMX installer before booting -- PID 1 does not install one");
    }
}

#if defined(OVMX_BOOT_ACP_BRIDGE)

/*
 * stage_boot_images - ACP-read bootstrap bridge (vms-5f0).
 *
 * The boot chain fork()+execve()s a small first-hop set of images, and the
 * host kernel maps each one's PT_LOAD and opens its PT_INTERP (IMGACT.EXE) BY
 * POSIX PATH before any OVMX code runs. With the /vms passthrough retired those
 * files have no POSIX home, so PID 1 reads each FROM THE GENUINE ODS-2 VOLUME
 * THROUGH THE EXECUTIVE ACP (ovmx_boot_acp_stage -> IO$_ACCESS + IO$_READVBLK)
 * and writes it into OVMX_BOOT_STAGE_DIR (a tmpfs). Every execve target that
 * names a SYS$SYSTEM image is then rewritten there (ovmx_boot_stage_exec_path).
 *
 * The bytes come from the ACP, never a /vms read; tmpfs is only the host-exec
 * handoff (the chicken-and-egg of activating image #1). Everything downstream
 * of the first hop -- shareables, data files -- flows through the ACP
 * in-process and is NOT staged here.
 *
 * SUBSTRATE-NEUTRAL (vms-8e8f): every line below is portable POSIX plus ACP
 * calls, so ONE stage_boot_images() serves Linux and NetBSD-vax alike
 * (INV-DRIFT). The single substrate-specific step -- where a writable staging
 * filesystem comes from -- is behind ovmx_boot_prepare_stage_dir() in the
 * ovmx_boot.h seam (Linux: two mkdirs on the initramfs tmpfs; NetBSD: the same
 * two mkdirs plus a tmpfs mount over the FFS root).
 *
 * KERNEL-BINFMT ENDGAME (note, not built here): a kernel binfmt that activates
 * a VMS image directly from the ACP would remove even this first-hop tmpfs. It
 * is post-flip work; this bridge is the boot-path realisation the flip ships.
 *
 * Called after require_installed_system() has confirmed (over the ACP) that the
 * volume is installed, so a staging read that fails here is an unexpected fault,
 * not the "blank volume" condition -- it halts honestly.
 */
static void stage_boot_images(void)
{
    static const char *const images[] = {
#if defined(OVMX_BOOT_LINUX)
        /* IMGACT.EXE is the PT_INTERP the kernel opens for each execve -- but
         * ONLY where OVMX images are IMGACT-activated. netbsd-vax activates
         * through NetBSD's own /usr/libexec/ld.elf_so (Decision A, vms-42d: no
         * OVMX-native VAX toolchain exists), so no VAX image carries an
         * IMGACT.EXE PT_INTERP and the shipped VAX system volume does not
         * carry the file at all. Demanding it there would halt every VAX boot
         * on a file that is correctly absent -- the opposite of fail-honest.
         * This keys off the SUBSTRATE macro, not the bridge macro, because it
         * is a statement about the activation model, not about this link. */
        "IMGACT.EXE",
#endif
        "PROVISION.EXE",   /* PID 1 forks this (the startup process)         */
        "DCL.EXE",         /* PROVISION execve's this on STARTUP.COM         */
        "JOB_CONTROL.EXE", /* RUN/DETACHED from the startup phase driver     */
        "LOGINOUT.EXE",    /* JOB_CONTROL execve's this for the console login*/
    };

    if (ovmx_boot_prepare_stage_dir(OVMX_BOOT_STAGE_DIR) != 0)
        ovmx_sysinit_halt("cannot create the boot-image staging directory",
                          strerror(errno));

    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        char acp_path[512], dest[512];
        snprintf(acp_path, sizeof(acp_path), "%s/%s", sysexe_linux, images[i]);
        snprintf(dest, sizeof(dest), "%s/%s", OVMX_BOOT_STAGE_DIR, images[i]);

        uint32_t st = ovmx_boot_acp_stage(acp_path, dest);
        if (!$VMS_STATUS_SUCCESS(st)) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "SYS$SYSTEM:%s could not be read from the ODS-2 volume "
                     "over the ACP (status %#x)", images[i], st);
            ovmx_sysinit_halt("boot-image staging failed", detail);
        }
    }

    /*
     * SYS$SYSTEM UTILITY IMAGES (vms-37e). The five mandatory images above are
     * the boot chain's own first hop. But DCL activates a SYS$SYSTEM utility
     * (INSTALL, SYSGEN, AUTHORIZE, MAIL, ...) via dcl_exec_utility(), which
     * fork()+execve()s the image so it can pass P1-P8 argv -- and in-process
     * ACP activation (imgact_activate) carries NO argv, so those utilities
     * cannot ride the in-process path and, exactly like the first hop, need a
     * POSIX home the kernel can execve now that the /vms passthrough is retired.
     * The bytes still come off the genuine ODS-2 volume THROUGH THE ACP (INV-6);
     * only the Linux-exec handoff lives in tmpfs. dcl_exec_utility() rewrites its
     * SYS$SYSTEM path here via ovmx_boot_stage_exec_path() when /vms is absent.
     *
     * BEST-EFFORT: unlike the first hop, a missing utility is NOT a boot-fatal
     * condition (the OS kit stages several of these with `2>/dev/null` -- an
     * install can legitimately omit one). ovmx_boot_acp_present() distinguishes
     * "absent" (skip, honest) from a genuine read fault (halt) so an absent
     * optional never masks a broken volume.
     */
    static const char *const utils[] = {
        "INSTALL.EXE", "SYSGEN.EXE", "AUTHORIZE.EXE", "MAIL.EXE",
        "MONITOR.EXE", "INITIALIZE.EXE", "PRODUCT.EXE", "LIBRARIAN.EXE",
        "HELP.EXE", "SCSD.EXE",
        /* OVMX-native toolchain (vms-104). The MMK self-host path defines
         * TCC :== $SYS$SYSTEM:TCC.EXE and LNK :== $SYS$SYSTEM:LINK.EXE (the
         * descrip.mms toolchain verbs) and fork()+execve()s each (a plain
         * static image is not in-process-eligible,
         * so dcl_activate_image forks it) -- exactly the SYSEXE-utility class
         * above, so they stage the SAME way: bytes off the genuine ODS-2 volume
         * through the ACP (INV-6), rewritten to the staged copy by
         * dcl_resolve_activatable_acp / dcl_exec_utility when /vms is absent.
         * BEST-EFFORT like the rest: a system disk that does not ship the
         * toolchain (a minimal, non-self-hosting install) skips them honestly.
         * LIBRARIAN.EXE is already staged above (it is also a SYSEXE utility). */
        "TCC.EXE", "LINK.EXE",
    };
    for (size_t i = 0; i < sizeof(utils) / sizeof(utils[0]); i++) {
        char acp_path[512], dest[512];
        snprintf(acp_path, sizeof(acp_path), "%s/%s", sysexe_linux, utils[i]);
        if (!ovmx_boot_acp_present(acp_path))
            continue;                 /* optional utility not installed: skip */
        snprintf(dest, sizeof(dest), "%s/%s", OVMX_BOOT_STAGE_DIR, utils[i]);
        uint32_t st = ovmx_boot_acp_stage(acp_path, dest);
        if (!$VMS_STATUS_SUCCESS(st)) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "SYS$SYSTEM:%s is present on the ODS-2 volume but could "
                     "not be read over the ACP (status %#x)", utils[i], st);
            ovmx_sysinit_halt("utility-image staging failed", detail);
        }
    }

    /*
     * SYS$SHARE SHAREABLE IMAGES (vms-0cb). SYSTARTUP_VMS.COM runs
     * `INSTALL ADD SYS$SHARE:*$SHR.EXE` to register the shareables every
     * dynamically-activated OVMX image depends on. INSTALL stat()s each one
     * to record its path in the Known Image DB -- but with the /vms
     * passthrough retired the SYS$SHARE shareables have no POSIX home, so
     * INSTALL failed %INSTALL-E-FILNOTFND for every one (image activation
     * itself still worked, via IMGACT's Priority-2 ACP fallback, but the
     * boot printed seven honest errors and the Known Image DB stayed empty).
     *
     * Same reroute class as the SYSEXE utilities above: PID 1 stages each
     * shareable off the genuine ODS-2 volume THROUGH THE ACP into the tmpfs
     * so INSTALL.EXE (which cannot ride the in-process ACP path -- it needs a
     * POSIX file to stat) resolves SYS$SHARE:<name> to the staged copy. The
     * bytes come from the ACP, never a /vms read (INV-6). BEST-EFFORT, like
     * the utilities: a shareable absent from the volume is skipped honestly
     * (INSTALL then reports its own %INSTALL-E-FILNOTFND for that one) rather
     * than masking a broken volume -- ovmx_boot_acp_present() distinguishes
     * "absent" from a genuine read fault. This list is exactly the shareable
     * set the distro SYSTARTUP_VMS.COM procedures INSTALL-ADD, and the
     * Dockerfile.bootable "9 VMS-native artifacts" gate ships.
     */
    static const char *const shareables[] = {
        "DECC$SHR.EXE", "LIBVMSSYS$SHR.EXE", "LIBVMS$SHR.EXE",
        "LIBVMSPROCESS$SHR.EXE", "LIBVMSLNM$SHR.EXE", "LIBVMSFS$SHR.EXE",
        "LIBVMSRMS$SHR.EXE",
    };
    for (size_t i = 0; i < sizeof(shareables) / sizeof(shareables[0]); i++) {
        char acp_path[512], dest[512];
        snprintf(acp_path, sizeof(acp_path), "%s/%s", syslib_linux, shareables[i]);
        if (!ovmx_boot_acp_present(acp_path))
            continue;                 /* optional shareable not installed: skip */
        snprintf(dest, sizeof(dest), "%s/%s", OVMX_BOOT_STAGE_DIR, shareables[i]);
        uint32_t st = ovmx_boot_acp_stage(acp_path, dest);
        if (!$VMS_STATUS_SUCCESS(st)) {
            char detail[256];
            snprintf(detail, sizeof(detail),
                     "SYS$SHARE:%s is present on the ODS-2 volume but could "
                     "not be read over the ACP (status %#x)", shareables[i], st);
            ovmx_sysinit_halt("shareable-image staging failed", detail);
        }
    }

    /*
     * SYS$SYSTEM:OVMXVMSSYS.PAR (vms-0cb). read_boot_parameters() reads the
     * SCSNODE SYSGEN parameter from this file through the shared sysgen_params.h
     * reader, which fopen()s a /vms path retired by the flip -- so the boot
     * fell back to the default node name with %OVMX-W-NOPARAMS where the
     * boot-console oracle expects %OVMX-I-SCSNODE. Stage the file off the ODS-2
     * volume over the ACP (bytes from the ACP, INV-6) so read_boot_parameters()
     * can point the reader at it (OVMX_SYSGEN_PATH). BEST-EFFORT: an absent or
     * unreadable .PAR keeps the honest NOPARAMS fallback (an already-installed
     * volume may legitimately lack it -- read_boot_parameters()'s header).
     */
    {
        char acp_path[512], dest[512];
        snprintf(acp_path, sizeof(acp_path), "%s/OVMXVMSSYS.PAR", sysexe_linux);
        if (ovmx_boot_acp_present(acp_path)) {
            snprintf(dest, sizeof(dest), "%s/OVMXVMSSYS.PAR", OVMX_BOOT_STAGE_DIR);
            (void)ovmx_boot_acp_stage(acp_path, dest);  /* NOPARAMS on failure */
        }
    }
}
#else  /* !OVMX_BOOT_ACP_BRIDGE */
/* No ACP-read bridge in this link: the pre-cutover NetBSD-vax runtime keeps
 * its current boot model (no ACP-staging tmpfs). The cutover is vms-329;
 * vms-8e8f already proved the body above cross-compiles ILP32-clean for
 * elf32-vax. Staging is a no-op here. */
static void stage_boot_images(void) { }
#endif  /* OVMX_BOOT_ACP_BRIDGE */

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
 * Unix sense. OVMX's underlying layer has no analogue of boot-driver I/O -- the
 * parameter file is an ordinary file ON the system disk, unreadable until
 * vmsfs has mounted it -- so this necessarily runs AFTER the mount and
 * AFTER the executive attaches. Named here as a platform limit, not hidden
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

#if defined(OVMX_BOOT_ACP_BRIDGE)
    /*
     * ATOMIC FLIP (vms-0cb): the shared SYSGEN reader (sysgen_params.h
     * sysgen_current_path) resolves SYS$SYSTEM:OVMXVMSSYS.PAR through
     * vmsfs_to_linux_path and fopen()s the resulting /vms path -- retired by
     * the flip, so the read failed and every boot fell to the default node
     * name with %OVMX-W-NOPARAMS (the boot-console conformance oracle expects
     * %OVMX-I-SCSNODE). PID 1 staged the file off the genuine ODS-2 volume
     * over the ACP (stage_boot_images; INV-6, bytes from the ACP); point the
     * reader at that staged copy via OVMX_SYSGEN_PATH. SCOPED TO THIS CALL and
     * unset immediately below -- read_boot_parameters() runs before
     * run_startup() forks PROVISION, so no child inherits a frozen params path
     * (which would defeat SYSGEN's on-disk versioning). A genuinely absent
     * staged file leaves the env unset and the honest NOPARAMS fallback
     * intact. A link without the bridge keeps its pre-cutover read (vms-329).
     */
    int staged_params = (access(OVMX_BOOT_STAGE_DIR "/OVMXVMSSYS.PAR", R_OK) == 0);
    if (staged_params)
        setenv("OVMX_SYSGEN_PATH", OVMX_BOOT_STAGE_DIR "/OVMXVMSSYS.PAR", 1);
#endif

    int have_node =
        (sysgen_read_string("SCSNODE", node, sizeof(node)) == 0 && node[0] != '\0');

#if defined(OVMX_BOOT_ACP_BRIDGE)
    if (staged_params)
        unsetenv("OVMX_SYSGEN_PATH");
#endif

    if (have_node) {
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

/* ------------------------------------------------------------------ */
/* Cluster SYSGEN parameters into the executive (FC-P0.10)             */
/* ------------------------------------------------------------------ */

/*
 * sysgen_str_into - copy a NUL-terminated string read off the parameter file
 * into a fixed, non-NUL-terminated wire field, clamped to its width. Shared
 * by SCSNODE and DISK_QUORUM below so the clamp-and-copy logic exists once.
 */
static void sysgen_str_into(const char *src, uint8_t *dst, size_t dstlen,
                            uint8_t *out_len)
{
    size_t n = strlen(src);
    if (n > dstlen)
        n = dstlen;
    memcpy(dst, src, n);
    *out_len = (uint8_t)n;
}

/*
 * load_cluster_sysgen_params - STARTUP.EXE's own case of SYSBOOT (FC-P0.10,
 * docs/plan-faithful-cluster-executive.md). Reads the cluster SYSGEN
 * parameters off SYS$SYSTEM:OVMXVMSSYS.PAR through the SAME shared reader
 * read_boot_parameters() above uses (sysgen_read_string/sysgen_read_param --
 * vms_sysgen.c, scsd.c and the DCL F$GETSYI lexicals already share it), plus
 * the CLUSTER_AUTHORIZE group/password (cluster_authorize_read()), and hands
 * the whole set to VMS_IOCTL_SYSGEN_LOAD (vms_kif_sysgen_load) so the
 * executive's real struct vms_cluster (vms_cluster_node()) carries it before
 * any later item starts the port (VMS_IOCTL_CLUSTER_START, FC-P0.11).
 *
 * Uses the SAME OVMX_SYSGEN_PATH staging scope as read_boot_parameters() (see
 * its header for why) so both readers see the identical parameter file this
 * boot. A parameter absent from the file reads as its honest zero (VAXCLUSTER
 * 0 = never, VOTES 0, ...) -- never a fabricated default (INV-6); the FACTORY
 * DEFAULTS a fresh install seeds the file with live in tools/vms_sysgen.c and
 * sysboot.c, not here. Called unconditionally, even when the file is missing:
 * an all-zero set is the honest "VAXCLUSTER never" state, not a skip.
 *
 * Does not consult conversational_boot_params: a SYSBOOT> SET on a cluster
 * parameter this session is not yet threaded through to this load (only
 * SCSNODE's hostname effect is, above) -- a disclosed limitation, not a
 * silent one; the persisted store's value stands until a later item extends
 * sysboot.h with a numeric working-set reader.
 *
 * SS$_BADPARAM (the negctl this ioctl enforces: VAXCLUSTER >= 1 with no
 * SCSNODE loaded) is logged loudly and honestly -- OVMX invents no VMS
 * message text for a condition no oracle capture grounds (Rule 10) -- and the
 * boot continues; the cluster stack simply never received a loaded identity
 * (a fact the READER of it stays not knowing anything the executive itself
 * does not honestly hold, not a symptom this function paints over).
 *
 * Returns the VAXCLUSTER value the executive actually COMMITTED -- 0 if the
 * ioctl was refused (SS$_BADPARAM) or otherwise failed, regardless of what
 * this boot's parameter file named, because a refused SYSGEN_LOAD leaves
 * vms_cluster_node()->params at its PRIOR (honest, zero) state. The FC-P0.11
 * caller (start_cluster_port() below) gates VMS_IOCTL_CLUSTER_START on THIS
 * return value, never on the locally-parsed `args.vaxcluster` -- the executive's
 * real committed state is the only honest thing to gate on (INV-6).
 */
static uint32_t load_cluster_sysgen_params(void)
{
    struct vms_sysgen_load_args args;
    struct cluster_authorize auth;
    char strval[SYSGEN_STRVAL_LEN];
    uint32_t u32, status;

#if defined(OVMX_BOOT_ACP_BRIDGE)
    int staged_params = (access(OVMX_BOOT_STAGE_DIR "/OVMXVMSSYS.PAR", R_OK) == 0);
    if (staged_params)
        setenv("OVMX_SYSGEN_PATH", OVMX_BOOT_STAGE_DIR "/OVMXVMSSYS.PAR", 1);
#endif

    memset(&args, 0, sizeof(args));

    if (sysgen_read_string("SCSNODE", strval, sizeof(strval)) == 0 && strval[0] != '\0')
        sysgen_str_into(strval, args.scsnode, sizeof(args.scsnode), &args.scsnode_len);

    /* SCSSYSTEMID: the generic numeric SYSGEN reader is a plain uint32_t
     * (sys_misc.c's SYI$_SCSSYSTEMID already reads it the same way), so the
     * wire's high word is always 0 today -- honest, not a truncation this
     * function invents; a wider store is a future item's concern. */
    if (sysgen_read_param("SCSSYSTEMID", &u32) == 0)
        args.scssystemid_lo = u32;
    if (sysgen_read_param("ALLOCLASS", &u32) == 0)
        args.alloclass = (uint8_t)u32;
    if (sysgen_read_param("VOTES", &u32) == 0)
        args.votes = (uint16_t)u32;
    if (sysgen_read_param("EXPECTED_VOTES", &u32) == 0)
        args.expected_votes = (uint16_t)u32;
    if (sysgen_read_param("VAXCLUSTER", &u32) == 0)
        args.vaxcluster = (uint8_t)u32;
    if (sysgen_read_param("LOCKDIRWT", &u32) == 0)
        args.lockdirwt = (uint8_t)u32;
    if (sysgen_read_param("QDSKVOTES", &u32) == 0)
        args.qdskvotes = (uint16_t)u32;
    if (sysgen_read_param("RECNXINTERVAL", &u32) == 0)
        args.recnxinterval = (uint16_t)u32;
    if (sysgen_read_param("TIMVCFAIL", &u32) == 0)
        args.timvcfail = (uint16_t)u32;
    if (sysgen_read_param("CLUSTER_CREDITS", &u32) == 0)
        args.cluster_credits = (uint16_t)u32;
    if (sysgen_read_param("NISCS_MAX_PKTSZ", &u32) == 0)
        args.niscs_max_pktsz = u32;
    if (sysgen_read_param("MSCP_LOAD", &u32) == 0)
        args.mscp_load = (uint8_t)u32;
    if (sysgen_read_param("MSCP_SERVE_ALL", &u32) == 0)
        args.mscp_serve_all = (uint8_t)u32;

    if (sysgen_read_string("DISK_QUORUM", strval, sizeof(strval)) == 0 && strval[0] != '\0')
        sysgen_str_into(strval, args.disk_quorum, sizeof(args.disk_quorum),
                        &args.disk_quorum_len);

    if (cluster_authorize_read(&auth) == 0) {
        args.auth_group = auth.group;
        sysgen_str_into(auth.password, args.auth_password,
                        sizeof(args.auth_password), &args.auth_password_len);
        args.auth_valid = 1;
    }

#if defined(OVMX_BOOT_ACP_BRIDGE)
    if (staged_params)
        unsetenv("OVMX_SYSGEN_PATH");
#endif

    status = vms_kif_sysgen_load(&args);
    if (status == SS$_BADPARAM) {
        fprintf(stderr,
                "%%OVMX-F-BADSYSGEN, VAXCLUSTER is %u but no SCSNODE is configured\n",
                (unsigned)args.vaxcluster);
        fprintf(stderr,
                "-OVMX-I-BADSYSGEN, cluster SYSGEN parameters were not loaded\n");
        return 0;
    }
    if (status != SS$_NORMAL) {
        fprintf(stderr,
                "%%OVMX-W-NOCLUSTER, cluster SYSGEN parameters were not loaded"
                " (status %#x)\n", (unsigned)status);
        return 0;
    }

    return (uint32_t)args.vaxcluster;
}

/*
 * start_cluster_port - VMS_IOCTL_CLUSTER_START (FC-P0.11, docs/plan-
 * faithful-cluster-executive.md). The P0 "port up" semantic ONLY: bring
 * PEA0: up so HELLOs flow -- not the CNXMAN join to MEMBER/STANDALONE
 * (FC-P3.9, once CNXMAN exists).
 *
 * `vaxcluster` is load_cluster_sysgen_params()'s return -- the value the
 * executive actually COMMITTED, never a locally re-parsed copy. The gate
 * itself (cluster_start_wanted(), cluster_boot_gate.h) is the ONE place the
 * VAXCLUSTER convention (0 = never, 1 = if present, 2 = always; non-zero
 * both start the port -- see that header's own rationale) is decided, shared
 * with its R1 host test so this boot path and the test can never drift.
 *
 * VAXCLUSTER=0: this function does not even issue the ioctl -- no PEA0:, no
 * HELLO, the honest silence INV-6 requires, not a call the executive then
 * has to refuse.
 */
static void start_cluster_port(uint32_t vaxcluster)
{
    uint32_t port_up = 0;
    uint32_t status;

    if (!cluster_start_wanted(vaxcluster))
        return;

    status = vms_kif_cluster_start(&port_up);
    if (status != SS$_NORMAL) {
        fprintf(stderr,
                "%%OVMX-W-CLUSTERPORT, cluster port did not start"
                " (VAXCLUSTER %u, status %#x)\n",
                (unsigned)vaxcluster, (unsigned)status);
        return;
    }
    if (!port_up) {
        /* Honest: SS$_NORMAL with the port still not up should not happen
         * (vms_pe_start's own contract), but INV-6 forbids ever printing a
         * success line the executive itself did not attest to. */
        fprintf(stderr,
                "%%OVMX-W-CLUSTERPORT, CLUSTER_START returned normally but"
                " PEA0: is not up\n");
    }
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
    /* ATOMIC FLIP (vms-5f0): execve the copy PID 1 staged off the ODS-2 volume
     * over the ACP into OVMX_BOOT_STAGE_DIR -- the /vms POSIX path no longer
     * exists for the kernel to map. The staged file IS the volume's bytes.
     * Self-guarding: use the staged copy only if it is actually present, so on
     * a substrate that did not stage (NetBSD-vax, vms-d5d) the original path is
     * kept unchanged. */
    {
        char staged[512];
        if (ovmx_boot_stage_exec_path(provision_path, staged, sizeof(staged)) &&
            access(staged, X_OK) == 0)
            snprintf(provision_path, sizeof(provision_path), "%s", staged);
    }

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
     * SYSKRNL identity line -- printed FIRST, before any mount, module
     * load, or VMS-flavored boot message. GNU/Linux-style dual identity
     * (operator ruling, rd vms-700/vms-296/vms-3de): "OVMX/Linux" names
     * the SYSKRNL layer (the Linux kernel underneath) this boots on,
     * distinct from and prior to the "OpenVMX" product identity STARTUP.EXE
     * hands off to later (display_boot_banner(), below) -- the boot reads
     * the "OVMX/Linux" SYSKRNL layer coming up, THEN "OpenVMX" product.
     * Unconditional (not gated on getpid() == 1): cheap, and every
     * invocation of this binary is still OVMX/Linux underneath regardless
     * of PID.
     */
    printf("%s\n", ovmx_syskrnl_banner());
    fflush(stdout);

    /* If we are PID 1 on bare metal (a fresh kernel whose base filesystems
     * are not yet mounted), set up the substrate plumbing. */
    if (getpid() == 1 && !ovmx_boot_kernel_filesystems_mounted()) {
        bare_metal_init();
    }

    /* No image runs without the executive. On bare metal this is already
     * satisfied (bare_metal_init attached it before INITIALIZE.EXE ran) and
     * this call is a no-op; on any other base it is the gate. Placing
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

    /* Step 2a: ACP-read bootstrap bridge (vms-5f0). Stage the first-hop
     * execve'd images (IMGACT/PROVISION/DCL/JOB_CONTROL/LOGINOUT) off the
     * genuine ODS-2 volume THROUGH THE EXECUTIVE ACP into OVMX_BOOT_STAGE_DIR,
     * so the Linux kernel can execve them now that the /vms POSIX passthrough
     * is retired. Must run after require_installed_system() (volume confirmed
     * installed over the ACP) and before run_startup() forks PROVISION.EXE. */
    stage_boot_images();

    /* Step 2b: SYSBOOT's job -- read SYS$SYSTEM:OVMXVMSSYS.PAR and set the
     * system's identity (sethostname) from its SCSNODE parameter (vms-b6a7,
     * docs/design-boot-faithful.md §4.1). Must run after Step 1b (the
     * device table / logical names that make VMS_PARAMS_PATH resolvable)
     * and after require_installed_system() confirms this is a real
     * installed volume, not a blank one. See read_boot_parameters()'s own
     * header for the ordering divergence from real VMS and the honest
     * missing/corrupt-file fallback. */
    read_boot_parameters();

    /* Step 2c: STARTUP.EXE's own case of SYSBOOT (FC-P0.10, docs/plan-
     * faithful-cluster-executive.md) -- load the cluster SYSGEN parameters
     * and the CLUSTER_AUTHORIZE record into the executive's real
     * struct vms_cluster, BEFORE any later item starts the cluster port
     * (VMS_IOCTL_CLUSTER_START, FC-P0.11), reproducing SYSBOOT's ordering
     * (vms_cluster.h section 2). Must run after read_boot_parameters()'s own
     * mount/device-table preconditions. */
    {
        uint32_t cluster_vaxcluster = load_cluster_sysgen_params();

        /* Step 2d: FC-P0.11 -- bring PEA0: up (P0 "port up" semantic only),
         * gated on the VAXCLUSTER value the executive just committed. Must
         * run immediately after Step 2c: SYSINIT's own ordering (design
         * SS3.5) is SYSGEN_LOAD, then CLUSTER_START, before the system disk
         * mount / STARTUP.COM below. */
        start_cluster_port(cluster_vaxcluster);
    }

    /* Step 3: %STDRV-I-STARTUP begun, then run STARTUP.COM.
     * The STDRV "begun" bracket precedes the startup procedure (F1). There is
     * NO "completed" counterpart anywhere in the boot (§3.5 oracle; the old
     * invented completed line stays deleted, vms-2f0/vms-1fb).
     *
     * There is no logical name daemon to start first: on VMS the logical name
     * tables are executive-resident, not a service (vms-a4b).
     *
     * run_startup() is ALSO where the system's services start, and the only
     * place they do: STARTUP.COM's phase driver either runs a registered
     * STDRV component directly (JOB_CONTROL at CONFIG, vms-2a9) or reaches
     * SYSTARTUP_VMS.COM at LPMAIN, which invokes each remaining service's
     * own startup procedure -- either way, each service is created with
     * RUN/DETACHED under a VMS process name (vms-47b). STARTUP.EXE
     * deliberately starts none of its own -- see the NOTE ON SERVICES
     * above. */
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
     * completion, which -- through its END-phase RUN_COMPONENTS step and
     * SYS$STARTUP:JOB_CONTROL_STARTUP.COM (vms-2a9: a registered STDRV
     * component, not a hardcoded call) -- has already created JOB_CONTROL as
     * a DETACHED process holding the console (vms-8d2, vms-47b's
     * RUN/DETACHED mechanism); by the time control reaches this line, the
     * console session is JOB_CONTROL's, not PID 1's, and grep for a login
     * loop here finds none.
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
