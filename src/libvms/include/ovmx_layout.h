/*
 * ovmx_layout.h - VMS System Disk Layout (VMS-Native Filespecs)
 *
 * All constants are VMS filespecs — no Linux paths except the single
 * SYSDISK_MOUNT used to bootstrap the device table.
 *
 * Consumers call vmsfs_to_linux_path() at the point of each syscall.
 * The device table (DKA0: → mount point) is populated at boot by
 * STARTUP.EXE and at session start by DCL.
 *
 * VMS Directory Hierarchy:
 *   [000000]                        Master File Directory (MFD)
 *   [SYS0]                          System root
 *   [SYS0.SYSCOMMON]                Common system tree
 *   [SYS0.SYSCOMMON.SYSEXE]        SYS$SYSTEM — executables, SYSUAF
 *   [SYS0.SYSCOMMON.SYSLIB]        SYS$LIBRARY/SYS$SHARE — libraries
 *   [SYS0.SYSCOMMON.SYSMGR]        SYS$MANAGER — admin configs
 *   [SYS0.SYSCOMMON.SYSHLP]        SYS$HELP — help files
 *   [SYS0.SYSCOMMON.SYS$STARTUP]   SYS$STARTUP — the startup driver's phase
 *                                   and component data files (vms-21a,
 *                                   docs/design-boot-faithful.md §3.2/§3.3)
 *   [USERS]                         User home directories
 *   [SYSTMP]                        Scratch / temporary files
 */

#ifndef __OVMX_LAYOUT_H
#define __OVMX_LAYOUT_H

/*
 * System disk mount point — the ONE Linux path in the entire system.
 * Used only to bootstrap the device table: DKA0: → SYSDISK_MOUNT.
 */
#define SYSDISK_MOUNT    "/vms"

/*
 * System disk device name.
 * Matches the device table entry that maps to SYSDISK_MOUNT.
 */
#define SYSDISK_DEVICE   "DKA0"

/*
 * Console terminal device name (vms-d0b).
 *
 * ORACLE-PINNED: SHOW TERMINAL on the console of both ~/vax OpenVMS
 * VAX V7.3 lab nodes prints the physical name "_OPA0:"
 * (docs/oracle/vax73-terminal-device.md §1).
 *
 * This is a NAME, not a claim about which terminal any given process is
 * on. The executive creates the unit under this name at module init
 * (src/kernel/vms_devtab.c, VMS_CONSOLE_DEVNAM); PID 1 uses it only to
 * $ASSIGN a channel when it creates the console login session. A
 * process asking WHICH TERMINAL IT IS ON must read the executive
 * ($GETJPI), never this constant -- picking the console because it is
 * the only terminal in the table is the exact inference vms-fb9 deleted.
 */
#define OVMX_CONSOLE_DEVICE  "OPA0:"

/* ------------------------------------------------------------------ */
/* VMS directory filespecs                                            */
/* ------------------------------------------------------------------ */

#define VMS_MFD          SYSDISK_DEVICE ":[000000]"
#define VMS_SYSROOT      SYSDISK_DEVICE ":[SYS0.SYSCOMMON]"
#define VMS_SYSEXE       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSEXE]"
#define VMS_SYSLIB       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSLIB]"
#define VMS_SYSMGR       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSMGR]"
#define VMS_SYSHLP       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSHLP]"
/*
 * SYS$STARTUP -- the startup driver's home directory (vms-21a). Measured
 * (docs/design-boot-faithful.md §3.2): the real logical is the SEARCH LIST
 * "SYS$SYSROOT:[SYS$STARTUP]" then "SYS$MANAGER". OVMX does not model
 * SYS$SYSROOT as a rooted logical that itself reaches SYSCOMMON (it flattens
 * every SYSCOMMON subdirectory directly off SYS$SYSDEVICE -- see
 * lnm_setup_defaults()'s own SYS$MANAGER, which is seeded the same way
 * instead of via SYS$SYSROOT:[SYSMGR]), so the first search-list element is
 * this flattened path rather than the oracle's literal "SYS$SYSROOT:
 * [SYS$STARTUP]" text -- STARTUP.COM's own DEFINE/SYSTEM documents this at
 * the point it runs.
 */
#define VMS_SYS_STARTUP  SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYS$STARTUP]"
#define VMS_USERS        SYSDISK_DEVICE ":[USERS]"
#define VMS_SYSTMP       SYSDISK_DEVICE ":[SYSTMP]"

/* ------------------------------------------------------------------ */
/* VMS filespecs for well-known system files                          */
/* ------------------------------------------------------------------ */

#define VMS_SYSUAF_PATH      "SYS$SYSTEM:SYSUAF.DAT"
#define VMS_LASTLOGIN_DIR    "SYS$MANAGER:LASTLOGIN"
/*
 * SET ACCOUNTING's enabled/disabled state (vms-17d, INV-DCL). Real OpenVMS
 * accounting on/off state is runtime, held by the accounting subsystem
 * itself (opening/closing SYS$MANAGER:ACCOUNTNG.DAT per class), not a
 * documented separate byte-format file -- so this flag FILE and its content
 * ("0"/"1", one line) are an OVMX invention (Rule 8), never presented as
 * VMS-authentic, that give OVMX's own accounting subsystem the same
 * property: system-wide, persisted, observable by any process, not the
 * per-DCL-context bool it replaces. See ovmx_accounting.h.
 */
#define VMS_ACCOUNTING_STATE_PATH "SYS$MANAGER:ACCOUNTNG.ENB"
#define VMS_OPERATOR_LOG     "SYS$MANAGER:OPERATOR.LOG"
#define VMS_SYLOGIN_PATH     "SYS$MANAGER:SYLOGIN.COM"
#define VMS_STARTUP_PATH     "SYS$MANAGER:STARTUP.COM"
/* Site-specific startup, invoked by STARTUP.COM. This is where services
 * are started (RUN/DETACHED under a VMS process name); STARTUP.EXE
 * starts none of its own -- see the NOTE ON SERVICES in
 * src/ovmx_init/ovmx_init.c (vms-47b). */
#define VMS_SYSTARTUP_PATH   "SYS$MANAGER:SYSTARTUP_VMS.COM"
/*
 * SYCONFIG.COM / SYLOGICALS.COM (vms-21a) -- the other two documented
 * OpenVMS site-customization startup procedures (VSI OpenVMS System
 * Manager's Manual, "Customizing Startup"), invoked by STARTUP.COM's phase
 * driver at CONFIG and BASEENVIRON respectively -- see STARTUP.COM's own
 * comments for the exact call sites. These REPLACE the Unix-shell-flavored
 * SYS$MANAGER:OVMX.CONF and SYS$MANAGER:SYLOGICALS.CONF (deleted, zero
 * readers, docs/design-boot-faithful.md §2.4): a real VMS site file is a
 * command PROCEDURE a manager edits with DCL commands in it, never a
 * KEY=VALUE config file. Both are seed-once on upgrade (vms-2c9,
 * tools/ovmx_kit_pack.c is_seed_once_filename()).
 */
#define VMS_SYCONFIG_PATH    "SYS$MANAGER:SYCONFIG.COM"
#define VMS_SYLOGICALS_PATH  "SYS$MANAGER:SYLOGICALS.COM"
#define VMS_HELPLIB_PATH     "SYS$HELP:HELPLIB.HLP"
/*
 * STDRV's phase-driver data files, under SYS$STARTUP: (vms-21a).
 *
 * VMS$PHASES.DAT -- the nine phase names, VERBATIM as measured
 * (docs/design-boot-faithful.md §3.3, Alpha 8.4 oracle capture): a plain
 * list of names, one per line, is a DATA FILE under Rule 8 -- reading it
 * during the lab capture was permitted and its content is OBSERVED, not
 * invented.
 *
 * VMS$VMS.DAT -- the phase -> component-procedure registration. Only the
 * FILE NAME is observed (§3.3's DIRECTORY listing); VSI's internal byte
 * format was never read and is not reproduced. The line format STARTUP.COM
 * parses ("PHASE-NAME procedure-filespec", one component per line) is an
 * OVMX INVENTION, labeled as such at every place that reads or writes it
 * (Rule 8) -- never presented as VMS-authentic.
 */
#define VMS_PHASES_DAT_PATH     "SYS$STARTUP:VMS$PHASES.DAT"
#define VMS_COMPONENTS_DAT_PATH "SYS$STARTUP:VMS$VMS.DAT"
#define VMS_SSH_HOST_KEY     "SYS$MANAGER:SSH_HOST_RSA_KEY"

/* ------------------------------------------------------------------ */
/* VMS filespecs for executables                                      */
/* ------------------------------------------------------------------ */

#define VMS_DCL_PATH         "SYS$SYSTEM:DCL.EXE"
/* PROVISION.EXE -- the OVMX startup process (vms-9b7). PID 1 execs this
 * where it used to exec DCL.EXE; it establishes the SYSTEM identity from
 * SYSUAF through the executive, provisions ownership and account home
 * directories, and then execs DCL.EXE on SYS$MANAGER:STARTUP.COM in the
 * SAME process, so STARTUP.COM runs as SYSTEM. It exists so that PID 1 --
 * which is statically linked and therefore grew two hand-rolled 512-byte
 * SYSUAF parsers of its own -- does not read SYSUAF at all. */
#define VMS_PROVISION_PATH   "SYS$SYSTEM:PROVISION.EXE"
#define VMS_LOGINOUT_PATH    "SYS$SYSTEM:LOGINOUT.EXE"
/* ZERO READERS as of vms-47b: its only reader was STARTUP.EXE's
 * start_sshd(), deleted with the rest of PID 1's service starting. SSH is
 * cancelled (vms-02d) as a startup-procedure target, and no startup
 * procedure names this constant -- but src/vmsssh/CMakeLists.txt still
 * builds VMSSSHD.EXE when libssh is present, so "no image of this name
 * is built" is not a claim this file can make. A service is started from
 * SYS$MANAGER:SYSTARTUP_VMS.COM with RUN/DETACHED, which takes its image
 * as a filespec in the procedure -- so a new service needs no constant
 * here. */
#define VMS_SSHD_PATH        "SYS$SYSTEM:VMSSSHD.EXE"
#define VMS_AUTHORIZE_PATH   "SYS$SYSTEM:AUTHORIZE.EXE"
#define VMS_INITIALIZE_PATH  "SYS$SYSTEM:INITIALIZE.EXE"

/* ------------------------------------------------------------------ */
/* VMS filespecs for data files                                       */
/* ------------------------------------------------------------------ */

#define VMS_RIGHTSLIST_PATH   "SYS$SYSTEM:RIGHTSLIST.DAT"
#define VMS_QMAN_DB          "SYS$MANAGER:QMAN$MASTER.DAT"
/* SYSGEN parameter file (vms-d34). NAME SHAPE and VERSIONING BEHAVIOR are
 * observed facts (docs/design-boot-faithful.md §3.4: the Alpha oracle's
 * SYS$SYSROOT:[SYSEXE]ALPHAVMSSYS.PAR;2 over ;1 -- WRITE creates a new
 * highest version, USE reads the highest). The BINARY LAYOUT behind this
 * name (struct sysgen_file / sysgen_param in sysgen_params.h) is an OVMX
 * INVENTION, not VMS-authentic -- CLAUDE.md Rule 8: ALPHAVMSSYS.PAR's
 * internal byte format is not published anywhere OVMX may read, so this is
 * OVMX's own representation, labeled as such at its definition. */
#define VMS_PARAMS_PATH      "SYS$SYSTEM:OVMXVMSSYS.PAR"

/* ------------------------------------------------------------------ */
/* Linux paths — derived from SYSDISK_MOUNT                           */
/*                                                                     */
/* Use these ONLY in code that runs before the VMS filespec translator */
/* is available (ovmx_init, early boot) or in contexts that need raw   */
/* Linux paths (execv, access, stat).  Prefer VMS filespecs + runtime */
/* translation everywhere else.                                        */
/* ------------------------------------------------------------------ */

#define VMS_SYSTEM_DIR   SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE"
#define VMS_LIBRARY_DIR  SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSLIB"
#define VMS_MANAGER_DIR  SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSMGR"
#define VMS_HELP_DIR     SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSHLP"
#define VMS_USERS_DIR    SYSDISK_MOUNT "/USERS"
#define VMS_TEMP_DIR     SYSDISK_MOUNT "/SYSTMP"

/*
 * The setuid-root mount(2)/umount(2) helper MOUNT/DISMOUNT fork+exec
 * (tools/vms_mount_helper.c, vms-651). NOT a VMS filespec and never named
 * to a user: DCL never shows this path, the same way it never shows the
 * console device name it derives internally. Lives outside the VMS tree
 * entirely (not under SYSDISK_MOUNT) because it must be reachable even
 * when no volume is mounted yet -- it is what MOUNT uses to mount one.
 */
#define VMS_MOUNT_HELPER_PATH "/sbin/vms_mount_helper"

/* ------------------------------------------------------------------ */
/* Boot-image staging tmpfs — the ACP-read bootstrap bridge (vms-5f0)  */
/* ------------------------------------------------------------------ */
/*
 * ATOMIC FLIP. SYS$DISK is now a genuine Files-11 (ODS-2) volume owned by
 * the executive ACP; the vmsfs_to_linux_path -> /vms POSIX passthrough is
 * retired. But the Linux kernel still activates a VMS image the Unix way:
 * execve() maps a MAIN image's PT_LOAD and opens its PT_INTERP (IMGACT.EXE)
 * BY POSIX PATH, before any OVMX code runs. The boot chain genuinely
 * fork()+execve()s a small set of images -- PROVISION.EXE (PID 1),
 * DCL.EXE (PROVISION), JOB_CONTROL.EXE (RUN/DETACHED), LOGINOUT.EXE
 * (JOB_CONTROL) -- plus the PT_INTERP IMGACT.EXE the kernel opens for each.
 * With /vms gone those files have no POSIX home.
 *
 * The bridge: PID 1 reads that first-hop image set FROM THE GENUINE ODS-2
 * VOLUME THROUGH THE EXECUTIVE ACP (vms_kif_acp_* / imgact_acp.c, IO$_ACCESS
 * + IO$_READVBLK) and writes each into this tmpfs, then every execve target
 * that names a SYS$SYSTEM image is rewritten here (ovmx_boot_stage_exec_path
 * below). tmpfs is ONLY the Linux-exec handoff (the chicken-and-egg of
 * activating image #1); the BYTES come from the ACP, never a /vms read, and
 * presence is never faked (INV-6). Everything downstream of the first hop --
 * shareables, data files -- flows through the ACP in-process, not here.
 *
 * KERNEL-BINFMT ENDGAME (NOTE, not built here): the deeper end state is a
 * kernel binfmt handler that activates a VMS image directly from the ACP,
 * so even the first-hop main image never needs a POSIX file. That removes
 * this tmpfs entirely. It is POST-boot-flip work; the bridge below is the
 * boot-path realisation the flip ships.
 */
#define OVMX_BOOT_STAGE_DIR  "/run/ovmx-boot"

/*
 * If `in` names a SYS$SYSTEM image (a ".EXE" whose path passes through the
 * SYSEXE directory), fill `out` with its boot-staging location
 * (OVMX_BOOT_STAGE_DIR "/" basename) and return 1. Otherwise return 0 and
 * leave `out` untouched, so the caller keeps the original path.
 *
 * Self-contained: no libc, because this header is included by freestanding
 * translation units (libvmssys). Callers pass it a resolved Linux path
 * (…/SYSEXE/NAME.EXE) or an equivalent and use the rewritten path as the
 * execve target.
 */
static inline int ovmx_boot_stage_exec_path(const char *in, char *out,
                                            unsigned long sz)
    __attribute__((unused));
static inline int ovmx_boot_stage_exec_path(const char *in, char *out,
                                            unsigned long sz)
{
    if (!in || !out || sz == 0)
        return 0;

    /* Case-insensitive uppercase of an ASCII byte (no libc/locale). */
#define OVMX_BOOT__UP(c) (((c) >= 'a' && (c) <= 'z') ? (char)((c) - 32) : (char)(c))

    /* Length of `in`, and basename = char after the last '/'. */
    unsigned long len = 0;
    const char *base = in;
    for (const char *p = in; *p; p++) {
        len++;
        if (*p == '/')
            base = p + 1;
    }

    /* Basename must end in ".EXE" -- case-insensitively, because the /vms
     * passthrough resolves the filename component in LOWERCASE (VMS specs are
     * case-insensitive) while the genuine ODS-2 volume (and the staged copies)
     * carry the UPPERCASE name. */
    unsigned long bl = 0;
    while (base[bl])
        bl++;
    if (bl < 4 || base[bl - 4] != '.' ||
        OVMX_BOOT__UP(base[bl - 3]) != 'E' ||
        OVMX_BOOT__UP(base[bl - 2]) != 'X' ||
        OVMX_BOOT__UP(base[bl - 1]) != 'E')
        return 0;

    /* Guard: only rewrite images that live in the SYSEXE directory, so a
     * non-system image path is never redirected into the boot staging dir.
     * Case-insensitive for the same reason. */
    int in_sysexe = 0;
    if (len >= 6) {
        for (unsigned long i = 0; i + 6 <= len; i++) {
            if (OVMX_BOOT__UP(in[i])     == 'S' &&
                OVMX_BOOT__UP(in[i + 1]) == 'Y' &&
                OVMX_BOOT__UP(in[i + 2]) == 'S' &&
                OVMX_BOOT__UP(in[i + 3]) == 'E' &&
                OVMX_BOOT__UP(in[i + 4]) == 'X' &&
                OVMX_BOOT__UP(in[i + 5]) == 'E') {
                in_sysexe = 1;
                break;
            }
        }
    }
    if (!in_sysexe)
        return 0;

    /* out = OVMX_BOOT_STAGE_DIR "/" UPPERCASE(basename) (bounded). The staged
     * copies PID 1 writes carry the volume's UPPERCASE name, so the rewritten
     * exec target must uppercase the (possibly lowercased) basename to match. */
    static const char pre[] = OVMX_BOOT_STAGE_DIR "/";
    unsigned long i = 0, j = 0;
    while (pre[j] && i + 1 < sz)
        out[i++] = pre[j++];
    j = 0;
    while (base[j] && i + 1 < sz) {
        char c = base[j++];         /* separate step: the macro re-reads its arg */
        out[i++] = OVMX_BOOT__UP(c);
    }
    out[i] = '\0';
    return 1;
#undef OVMX_BOOT__UP
}

/*
 * PER-USER PRIVATE STAGING (vms-a86f).
 *
 * LOGINOUT setuid()s every VMS session onto its SYSUAF UIC, so a session is a
 * NON-ROOT process (SYSTEM is UIC [1,4] -> uid 4). The shared boot-staging
 * directory OVMX_BOOT_STAGE_DIR is root-owned 0755 (PID 1 creates it and
 * pre-stages the boot images + a fixed utility set into it), so a non-root
 * session CANNOT write it -- lazily staging an image the boot bridge did not
 * pre-stage (e.g. SYS$SYSTEM:PARTS.EXE the first time `$ PARTS` runs) fails
 * EACCES. Making the shared directory world-writable would be a plant hole
 * (any user could drop an image others activate) and is forbidden.
 *
 * The faithful fix: each session lazily stages into a directory IT OWNS,
 * OVMX_BOOT_STAGE_DIR "/<uid>/", created 0700. This helper computes that
 * per-user destination for a SYS$SYSTEM image path exactly as
 * ovmx_boot_stage_exec_path computes the shared one, but with the "<uid>/"
 * component inserted before the (uppercased) basename. Returns 1 and fills
 * `out` iff `in` names a SYS$SYSTEM ".EXE"; otherwise returns 0.
 *
 * `ovmx_boot_stage_user_dir` fills `out` with just the per-user directory
 * (OVMX_BOOT_STAGE_DIR "/<uid>"), for the caller to mkdir(0700).
 *
 * Freestanding (no libc): the caller passes getuid() as `uid`. imgsrc_map_
 * staged() in the activator maps EITHER staged shape (shared or per-user) back
 * to the on-volume path by taking the last path component, so the genuine
 * image bytes are still read off the ODS-2 volume over the ACP (INV-6).
 */
static inline int ovmx_boot_stage_user_dir(char *out, unsigned long sz,
                                           unsigned long uid)
    __attribute__((unused));
static inline int ovmx_boot_stage_user_dir(char *out, unsigned long sz,
                                           unsigned long uid)
{
    if (!out || sz == 0)
        return 0;
    static const char pre[] = OVMX_BOOT_STAGE_DIR "/";
    unsigned long i = 0, j = 0;
    while (pre[j] && i + 1 < sz)
        out[i++] = pre[j++];
    /* decimal uid, no libc */
    char ub[24];
    int un = 0;
    unsigned long u = uid;
    if (u == 0) {
        ub[un++] = '0';
    } else {
        char tmp[24];
        int t = 0;
        while (u && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (u % 10)); u /= 10; }
        while (t) ub[un++] = tmp[--t];
    }
    for (int k = 0; k < un && i + 1 < sz; k++)
        out[i++] = ub[k];
    out[i < sz ? i : sz - 1] = '\0';
    return 1;
}

static inline int ovmx_boot_stage_user_path(const char *in, char *out,
                                            unsigned long sz, unsigned long uid)
    __attribute__((unused));
static inline int ovmx_boot_stage_user_path(const char *in, char *out,
                                            unsigned long sz, unsigned long uid)
{
    if (!in || !out || sz == 0)
        return 0;

    /* Reuse the shared computation for the guard (SYSEXE + ".EXE") and the
     * uppercased basename, then rebuild with the "<uid>/" component. */
    char shared[512];
    if (!ovmx_boot_stage_exec_path(in, shared, sizeof shared))
        return 0;

    /* basename = char after the last '/' of the shared path. */
    const char *base = shared;
    for (const char *p = shared; *p; p++)
        if (*p == '/')
            base = p + 1;

    unsigned long i = 0;
    if (!ovmx_boot_stage_user_dir(out, sz, uid))
        return 0;
    while (out[i])
        i++;
    if (i + 1 < sz)
        out[i++] = '/';
    unsigned long j = 0;
    while (base[j] && i + 1 < sz)
        out[i++] = base[j++];
    out[i < sz ? i : sz - 1] = '\0';
    return 1;
}

#endif /* __OVMX_LAYOUT_H */
