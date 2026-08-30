/*
 * lnm_defaults.c - Default VMS System Logical Names
 *
 * Sets up the standard VMS logical names that every system needs.
 * These correspond to the logicals defined by SYSGEN on a real
 * VMS system and are placed into the SYSTEM table.
 *
 * All equivalence strings use VMS notation (device:[directory] specs),
 * never Linux paths.  The single mapping from VMS device names to
 * Linux mount points lives in the device table (vmsfs_device_add).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>

#include "vms/logical.h"
#include "ovmx_layout.h"
#include "ssdef.h"

/*
 * lnm_seed_system_locating - Define a SYSTEM logical, with a host-tooling
 * process-scope fallback (vms-48ab, INV-6-preserving).
 *
 * At OVMX runtime, PID 1 pins /dev/vms (Rule 9), so the LNM$SYSTEM create
 * below reaches the executive and that is the whole story: every process on
 * the node sees the SAME system-wide value, exactly as VMS requires, and
 * this function does nothing further.
 *
 * Under host BUILD/TEST tooling there is no executive, so vms-48ab's
 * SS$_NOSUCHDEV comes back honestly. THIS FUNCTION DOES NOT ADD A FALLBACK
 * TO THE REMOVED ONE: lnm_create/lnm_translate/lnm_delete against
 * LNM$SYSTEM itself still have none, so an explicit LNM$SYSTEM lookup
 * (SHOW LOGICAL/SYSTEM, F$TRNLNM(name,"LNM$SYSTEM")) still honestly reports
 * SS$_NOSUCHDEV -- see tests/vmslnm/test_vmslnm.c's
 * test_system_table_no_fallback() and tests/qemu/test_syssvc_lnm_system.c's
 * no-executive branch, neither of which this function touches.
 *
 * What it does instead, only on that SS$_NOSUCHDEV, is define the SAME name
 * with the SAME value in LNM$PROCESS_TABLE -- a real, disclosed, per-process
 * VMS logical, not a disguise of the SYSTEM table (SHOW LOGICAL reports it
 * as "(LNM$PROCESS_TABLE)", never as SYSTEM or shared). This is exactly the
 * thing a sysadmin can always do on real VMS -- a process-level DEFINE
 * shadows a system-level one -- so host tooling (ctest binaries, LOGINOUT.
 * EXE's own authenticate step) can still locate SYS$SYSTEM:SYSUAF.DAT,
 * SYS$SYSTEM:RIGHTSLIST.DAT, etc. through the default LNM$FILE_DEV search
 * order, which checks PROCESS before SYSTEM. It never fires when the
 * executive is present, so real multi-process SYSTEM-table semantics (a
 * sysadmin's system-wide DEFINE/SYSTEM taking effect for every OTHER
 * process) are completely unaffected.
 */
static void lnm_seed_system_locating(lnm_manager_t *mgr, const char *name,
                                     const char *value, uint32_t attr)
{
    uint32_t st = lnm_create(mgr, LNM_SYSTEM_TABLE, name, value,
                             attr, LNM_MODE_EXEC);
    if (st == SS$_NOSUCHDEV)
        lnm_create(mgr, LNM_PROCESS_TABLE, name, value, attr, LNM_MODE_EXEC);
}

/*
 * lnm_seed_system_locating_multi - the search-list (multi-value) counterpart
 * of lnm_seed_system_locating(), for the concealed rooted SYS$SYSROOT and the
 * DECC$LIBRARY_INCLUDE header search list. Same INV-6-preserving contract:
 * define the name node-wide in LNM$SYSTEM through the executive, and only when
 * that comes back SS$_NOSUCHDEV (host BUILD/TEST tooling, no /dev/vms) fall
 * back to the SAME disclosed value in LNM$PROCESS_TABLE (a real per-process
 * logical, never a disguise of the SYSTEM table -- see the block comment on
 * lnm_seed_system_locating above). lnm_create_multi carries the search list
 * AND the attributes (e.g. LNM$M_CONCEALED) to the executive (vms-420/#284).
 */
static void lnm_seed_system_locating_multi(lnm_manager_t *mgr, const char *name,
                                           const char **values, int nvalues,
                                           uint32_t attr)
{
    uint32_t st = lnm_create_multi(mgr, LNM_SYSTEM_TABLE, name, values,
                                   nvalues, attr, LNM_MODE_EXEC);
    if (st == SS$_NOSUCHDEV)
        lnm_create_multi(mgr, LNM_PROCESS_TABLE, name, values, nvalues,
                         attr, LNM_MODE_EXEC);
}

/*
 * lnm_system_root_token - the node's system-root token ("0","1","11",...): the
 * [SYSn] root this node boots from on the (possibly shared) system disk.
 *
 * In OpenVMS the boot root is an INDEPENDENT boot property -- the SYSBOOT R5
 * root / the boot command's root number, assigned per node by CLUSTER_CONFIG --
 * NOT a function of SCSSYSTEMID. Measured on the reference lab (vms-a01): sysids
 * 1025/1026/1027 boot roots SYS0/SYS1/SYS11, which is no arithmetic relation
 * (sysid & 1023 = 1/2/3, not 0/1/11); and there is no sysid->root mapping in the
 * tree. OVMX therefore surfaces the root index the same DISCLOSED way it
 * surfaces the system DEVICE (OVMX_SYSDEVICE, vms-47d): the boot chain publishes
 * OVMX_SYSROOT_INDEX; absent it, default "0" (SYS0), byte-identical to the
 * historical single-root behaviour.
 *
 * NB: OVMX_SYSROOT_INDEX-via-env is the DERIVATION MECHANISM (proven by
 * tests/vmsrms/test_sysroot_pernode.c). Production per-node wiring -- a real
 * cluster deriving its index at boot from a SYSGEN param / CLUSTER_CONFIG, not
 * an env var -- is the deferred family-2 follow-on (rd vms-a01). Rule 8: the
 * index TRANSPORT is an OVMX design choice, not a VMS-authentic byte format.
 *
 * Oracle (docs/oracle/vax73-system-root-logicals.md, live VAX V7.3): node N sees
 * SYS$SYSROOT = "dev:[SYSn.]","SYS$COMMON:" and SYS$COMMON =
 * "dev:[SYSn.SYSCOMMON.]" -- both the node root and the common carry the per-node
 * token n. (The authentic SYS$SYSROOT member-2 is the LOGICAL "SYS$COMMON:", not
 * a literal path; keeping the shipped literal composition is a separate fidelity
 * follow-on, rd vms-a01.)
 *
 * The token is validated as 1-4 hex digits (the VMS root range [SYS0]..[SYSFFFF])
 * so a stray value can never inject into the composed filespec; anything else
 * falls back to "0".
 */
static const char *lnm_system_root_token(void)
{
    const char *tok = getenv("OVMX_SYSROOT_INDEX");
    if (tok && tok[0]) {
        size_t n = strlen(tok);
        if (n >= 1 && n <= 4) {
            size_t i;
            for (i = 0; i < n; i++)
                if (!isxdigit((unsigned char)tok[i]))
                    break;
            if (i == n)
                return tok;   /* valid: "0","1","11","1A",... */
        }
    }
    return "0";               /* SYS0 -- unchanged default */
}

/*
 * lnm_setup_defaults - Set up standard VMS system logicals.
 *
 * @mgr:      Manager instance
 * @vms_root: Root directory for VMS file tree (Linux mount point)
 *
 * First registers the system device in the device table (the ONE place a Unix
 * path is stored), then creates logicals with VMS equivalences (SYS$SYSDEVICE
 * is the DISCOVERED boot device -- VDA0: on virtio, DUA0: on VAX MSCP):
 *
 *   SYS$SYSDEVICE -> (discovered, e.g. VDA0:)
 *   SYS$SYSROOT   -> SYS$SYSDEVICE:[SYS0.], SYS$SYSDEVICE:[SYS0.SYSCOMMON.]
 *                    (concealed rooted search list)
 *   SYS$COMMON    -> SYS$SYSDEVICE:[SYS0.SYSCOMMON.]  (concealed rooted)
 *   SYS$SYSTEM    -> SYS$SYSROOT:[SYSEXE]   (composed, not a spelled-out path)
 *   SYS$LIBRARY   -> SYS$SYSROOT:[SYSLIB]
 *   SYS$SHARE     -> SYS$SYSROOT:[SYSLIB]
 *   SYS$MANAGER   -> SYS$SYSROOT:[SYSMGR]
 *   SYS$HELP      -> SYS$SYSROOT:[SYSHLP]
 *   SYS$SCRATCH   -> SYS$SYSDEVICE:[SYSTMP]
 *   SYS$LOGIN     -> SYS$SYSDEVICE:[USERS]  (overridden per-process by login)
 *   SYS$DISK      -> SYS$SYSDEVICE  (process default device)
 *   TT            -> /dev/tty     (the ONE Linux path for terminal I/O)
 *   SYS$INPUT     -> TT:
 *   SYS$OUTPUT    -> TT:
 *   SYS$ERROR     -> TT:
 *   SYS$COMMAND   -> TT:
 */
void lnm_setup_defaults(lnm_manager_t *mgr, const char *vms_root)
{
    if (!mgr)
        return;

    if (!vms_root)
        vms_root = SYSDISK_MOUNT;

    /*
     * System disk device logical.
     * SYS$SYSDEVICE is the system disk — everything derives from here (SYS$SYSROOT
     * and every SYS$xxx directory logical compose onto it).
     *
     * DEVICE-NATIVE NAMING (vms-47d / vms-104): the system device is DISCOVERED,
     * not a hard literal. The boot chain / the image activator publish the unit
     * the system volume is mounted on as OVMX_SYSDEVICE (imgact.c reads the same
     * variable, rung iii); honour it here so SYS$SYSTEM:/SYS$SHARE: resolve over
     * the Files-11 ACP to the ACTUAL mounted system volume (e.g. VDA300: on the
     * self-host toolchain harness) rather than a compile-time guess. Absent the
     * variable, fall back to the substrate's compile-time SYSDISK_DEVICE default
     * (VDA0: on virtio, DUA0: on VAX MSCP -- vms-9f5), never a hard DKA0: literal.
     */
    {
        const char *sysdev = getenv("OVMX_SYSDEVICE");
        lnm_seed_system_locating(mgr, "SYS$SYSDEVICE",
                                 (sysdev && sysdev[0]) ? sysdev : SYSDISK_DEVICE ":",
                                 LNM_ATTR_TERMINAL);
    }

    /*
     * SYS$SYSROOT -> the system root, as a CONCEALED ROOTED SEARCH LIST, exactly
     * as OpenVMS defines it: the node-specific root [SYS0.] overlaid on the
     * cluster-common root [SYS0.SYSCOMMON.]. Every SYS$xxx directory logical
     * below composes onto this (SYS$SYSROOT:[SYSEXE] etc.), so a file is looked
     * up first in the node-specific root and then in the common root -- the
     * running-system search order a real VMS system disk presents (VSI OpenVMS
     * System Manager's Manual, Vol. 1, "The System Disk" / rooted-directory
     * structure: SYS$SYSROOT is a concealed, rooted, search-list logical whose
     * members are the per-node root and SYS$COMMON; VSI OpenVMS User's Manual,
     * "Rooted Directories" and DCL Dictionary, DEFINE
     * /TRANSLATION_ATTRIBUTES=CONCEALED). LNM$M_CONCEALED marks it a concealed
     * device so filespec composition merges [SYS0.] + [SYSEXE] structurally
     * (vms-d8e/#340), and the two members fan out at translation time
     * (vms-b12/#332, extended for a concealed rooted root by vms-69e7): the
     * physical files live under [SYS0.SYSCOMMON.*], so the common member wins.
     * Nothing here is pre-flattened -- the SYS$xxx paths are DERIVED live.
     */
    /*
     * vms-a01: the node-specific root [SYSn.] and the common root
     * [SYSn.SYSCOMMON.] carry this node's boot-root token (default "0" -> SYS0,
     * byte-identical to the historical single-root default). Oracle-grounded on
     * live VAX V7.3 -- SYS$SYSROOT member 0 = "dev:[SYSn.]" and SYS$COMMON =
     * "dev:[SYSn.SYSCOMMON.]" both move with the node index n
     * (docs/oracle/vax73-system-root-logicals.md).
     */
    const char *root_tok = lnm_system_root_token();
    char sysroot_self[64];
    char syscommon_root[64];
    snprintf(sysroot_self, sizeof sysroot_self,
             "SYS$SYSDEVICE:[SYS%s.]", root_tok);
    snprintf(syscommon_root, sizeof syscommon_root,
             "SYS$SYSDEVICE:[SYS%s.SYSCOMMON.]", root_tok);
    const char *sysroot_members[2] = { sysroot_self, syscommon_root };
    lnm_seed_system_locating_multi(mgr, "SYS$SYSROOT", sysroot_members, 2,
                                   LNM_ATTR_CONCEALED);

    /*
     * SYS$COMMON -> the cluster-common root [SYS0.SYSCOMMON.], concealed and
     * rooted, exactly as VMS defines it (SHOW LOGICAL SYS$COMMON on a running
     * system: "dev:[SYS0.SYSCOMMON.]"; VSI OpenVMS System Manager's Manual,
     * Vol. 1, "The System Disk"). It is the second member of SYS$SYSROOT and a
     * composition base in its own right (e.g. DECC$LIBRARY_INCLUDE below).
     */
    lnm_seed_system_locating(mgr, "SYS$COMMON",
                             syscommon_root,
                             LNM_ATTR_CONCEALED);

    /*
     * System directory logicals — COMPOSED onto SYS$SYSROOT, not spelled out.
     * SYS$SYSTEM = SYS$SYSROOT:[SYSEXE] and so on, so each resolves through the
     * concealed rooted search list above (VSI OpenVMS System Manager's Manual,
     * Vol. 1: SYS$SYSTEM = SYS$SYSROOT:[SYSEXE], SYS$LIBRARY = SYS$SYSROOT:
     * [SYSLIB], SYS$MANAGER = SYS$SYSROOT:[SYSMGR], SYS$HELP = SYS$SYSROOT:
     * [SYSHLP]). Redefining SYS$SYSROOT (or its underlying SYS$SYSDEVICE) moves
     * all of them at once -- the proof that composition is live, not memorized.
     */

    /* SYS$SYSTEM -> SYS$SYSROOT:[SYSEXE] */
    lnm_seed_system_locating(mgr, "SYS$SYSTEM", "SYS$SYSROOT:[SYSEXE]", 0);

    /* SYS$LIBRARY -> SYS$SYSROOT:[SYSLIB] */
    lnm_seed_system_locating(mgr, "SYS$LIBRARY", "SYS$SYSROOT:[SYSLIB]", 0);

    /* SYS$SHARE -> SYS$SYSROOT:[SYSLIB] (same as SYS$LIBRARY on VMS) */
    lnm_seed_system_locating(mgr, "SYS$SHARE", "SYS$SYSROOT:[SYSLIB]", 0);

    /* SYS$MANAGER -> SYS$SYSROOT:[SYSMGR] */
    lnm_seed_system_locating(mgr, "SYS$MANAGER", "SYS$SYSROOT:[SYSMGR]", 0);

    /*
     * SYS$UPDATE -> SYS$SYSROOT:[SYSUPD], the standard VMS home for pre-PCSI
     * layered-product install kits and their SETUP.COM procedures -- e.g.
     * @SYS$UPDATE:PARTS_SETUP.COM (vms-977/vms-2579, the 0.2 demo). Composed
     * onto SYS$SYSROOT like the rest; seeded into the executive-resident SYSTEM
     * table here so a login process sees it even before SYS$MANAGER:STARTUP.COM's
     * DEFINE/SYSTEM runs; STARTUP.COM then (re)defines the same value, an
     * idempotent supersede.
     */
    lnm_seed_system_locating(mgr, "SYS$UPDATE", "SYS$SYSROOT:[SYSUPD]", 0);

    /* SYS$HELP -> SYS$SYSROOT:[SYSHLP] */
    lnm_seed_system_locating(mgr, "SYS$HELP", "SYS$SYSROOT:[SYSHLP]", 0);

    /*
     * DECC$LIBRARY_INCLUDE -> the C RTL system-header search list (VSI C User's
     * Guide, "Header-File Searching": the compiler consults DECC$LIBRARY_INCLUDE
     * for the shipped standard headers; VSI C RTL Reference Manual). A real
     * search list over the library directory, composed through SYS$SYSROOT and
     * SYS$COMMON so it, too, resolves through the concealed rooted chain rather
     * than a memorized path. Absent before vms-69e7.
     */
    static const char *decc_include_members[] = {
        "SYS$SYSROOT:[SYSLIB]",
        "SYS$COMMON:[SYSLIB]"
    };
    lnm_seed_system_locating_multi(mgr, "DECC$LIBRARY_INCLUDE",
                                   decc_include_members, 2, 0);

    /* SYS$SCRATCH -> system temp directory */
    lnm_seed_system_locating(mgr, "SYS$SCRATCH", "SYS$SYSDEVICE:[SYSTMP]", 0);

    /*
     * Default queue logicals (vms-f89). On OpenVMS SYS$PRINT and SYS$BATCH are
     * LOGICAL NAMES for the default print/batch queues, not literal queue
     * names baked into PRINT/SUBMIT: a site does DEFINE/SYSTEM SYS$PRINT
     * <queue> to redirect where a bare PRINT (or SUBMIT) sends its job. Seeded
     * to the same-named default queue ensure_queue_init() creates
     * (src/vmsdcl/dcl_cmd_process.c); PRINT/SUBMIT translate the logical at
     * point of use, so a later DEFINE takes effect live.
     *
     * Doc: VSI OpenVMS DCL Dictionary, PRINT (/QUEUE default SYS$PRINT) and
     * SUBMIT (/QUEUE default SYS$BATCH); VSI OpenVMS System Manager's Manual,
     * Vol. 1, "Managing Queues" (SYS$PRINT / SYS$BATCH as the default queue
     * logical names). These are non-terminal so an equivalence that is itself
     * another logical still translates further, matching VMS.
     */
    lnm_seed_system_locating(mgr, "SYS$PRINT", "SYS$PRINT", 0);
    lnm_seed_system_locating(mgr, "SYS$BATCH", "SYS$BATCH", 0);

    /*
     * SYS$TIMEZONE_NAME / SYS$TIMEZONE_RULE / SYS$TIMEZONE_DIFFERENTIAL are
     * ALSO seeded from the live system TZ (vms-f89), but in the DCL session
     * setup (src/vmsdcl/dcl_main.c) rather than here: they need <time.h> (TDF
     * from the current zone), and this file is built by the native-link path
     * (src/vmslink/mk_vmslnm_shr.sh) with a freestanding CFLAGS set that does
     * not guarantee the libc time extensions. lib_datetime.c reads
     * SYS$TIMEZONE_DIFFERENTIAL at point of use.
     */

    /*
     * Per-user logicals — defaults in SYSTEM table,
     * overridden per-process by login.
     */

    /* SYS$LOGIN -> default user area (overridden per-process) */
    lnm_seed_system_locating(mgr, "SYS$LOGIN", "SYS$SYSDEVICE:[USERS]", 0);

    /*
     * SYS$DISK, the terminal and the I/O channel logicals are PER-PROCESS on
     * OpenVMS (LNM$PROCESS_TABLE): SYS$DISK is the process default device, and
     * SYS$INPUT/OUTPUT/ERROR/COMMAND point at THIS process's terminal, which
     * differs between jobs (an interactive session vs. a batch job reading a
     * command file). Seeding them into the process table is both correct and
     * required now that LNM$SYSTEM is executive-resident and node-wide: a
     * terminal logical placed in SYSTEM by one process would wrongly bind every
     * other process on the node to this process's /dev/tty.
     */

    /* SYS$DISK -> system device (process default device) */
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$DISK", "SYS$SYSDEVICE",
               0, LNM_MODE_EXEC);

    /*
     * Terminal device — the ONE Linux path for I/O.
     * On real VMS this would be the physical terminal (TTA0:, VTA123:).
     * All I/O channel logicals point here via "TT:".
     */
    const char *tty = ttyname(STDIN_FILENO);
    if (!tty)
        tty = "/dev/tty";
    lnm_create(mgr, LNM_PROCESS_TABLE, "TT", tty,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /*
     * I/O channel logicals — point at TT:, not Linux /dev/ paths.
     * On real VMS these are per-process and may differ (e.g. batch
     * jobs get SYS$INPUT pointing at a command file).  For now
     * they all resolve through TT: → /dev/tty.
     */
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$INPUT", "TT:",
               0, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$OUTPUT", "TT:",
               0, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$ERROR", "TT:",
               0, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$COMMAND", "TT:",
               0, LNM_MODE_EXEC);
}

/*
 * lnm_define_login_logicals - per-user identity logicals from SYSUAF (vms-e48).
 *
 * WHAT THIS REPLACES: SYS$LOGIN/SYS$SCRATCH used to be Linux environment
 * variables -- vms_login setenv'd them and DCL getenv'd them -- so
 * F$TRNLNM("SYS$LOGIN") never saw the user's real home and returned the
 * generic SYS$SYSDEVICE:[USERS] default lnm_setup_defaults() seeds. These are
 * now REAL LNM$PROCESS logicals created through the same lnm_create() path
 * DEFINE uses, so translation is truthful.
 *
 * Contract and doc citations are on the declaration in vms/logical.h.
 */
/* lnm_create() reports a replaced name with SS$_SUPERSEDE (844), which is an
 * EVEN success code -- so plain (status & 1) would read a supersede as a
 * failure. SYS$LOGIN et al. deliberately supersede the generic defaults
 * lnm_setup_defaults() seeded, so both codes are success here (the same pair
 * dcl_translate_logical() accepts). */
#define LNM_LOGIN_OK(st)  ((st) == SS$_NORMAL || (st) == SS$_SUPERSEDE)

uint32_t lnm_define_login_logicals(lnm_manager_t *mgr, const char *table_name,
                                   const char *default_dir)
{
    if (!mgr || !table_name || !table_name[0] || !default_dir || !default_dir[0])
        return SS$_BADPARAM;

    /* SYS$LOGIN: the account's full default device + directory. Supersedes the
     * generic [USERS] default seeded elsewhere; the LNM$FILE_DEV search order
     * (PROCESS -> JOB -> GROUP -> SYSTEM) reaches this table before the SYSTEM
     * default. */
    uint32_t st = lnm_create(mgr, table_name, "SYS$LOGIN",
                             default_dir, 0, LNM_MODE_SUPER);
    if (!LNM_LOGIN_OK(st))
        return st;

    /* SYS$LOGIN_DEVICE: the device field of default_dir -- everything up to
     * and including the first ':'. Only defined when default_dir names a
     * device (it always does when it comes from a SYSUAF record). */
    const char *colon = strchr(default_dir, ':');
    if (colon) {
        size_t devlen = (size_t)(colon - default_dir) + 1;   /* keep the ':' */
        char device[LNM_MAX_VALUE + 1];
        if (devlen > LNM_MAX_VALUE)
            devlen = LNM_MAX_VALUE;
        memcpy(device, default_dir, devlen);
        device[devlen] = '\0';
        st = lnm_create(mgr, table_name, "SYS$LOGIN_DEVICE",
                        device, 0, LNM_MODE_SUPER);
        if (!LNM_LOGIN_OK(st))
            return st;
    }

    /*
     * SYS$SCRATCH IS DELIBERATELY NOT REDEFINED HERE (vms-e48 round 3).
     *
     * On OpenVMS SYS$SCRATCH defaults to the login directory (= SYS$LOGIN),
     * but a site may DEFINE it to a dedicated scratch area, and OVMX ships
     * exactly that: lnm_setup_defaults() defines SYS$SCRATCH system-wide to
     * SYS$SYSDEVICE:[SYSTMP] -- a directory the boot image masters
     * SYSTEM-writable (distro/Dockerfile.bootable; the vms-e5c/vms-221
     * executive fix that made SYS$SCRATCH: genuinely sys$create-able). Every
     * process on the node -- an interactive DCL and the images it activates
     * alike -- resolves that one system-wide value, so scratch files land in
     * one writable, consistent place.
     *
     * Overriding it per-login to = SYS$LOGIN would point it at each account's
     * home, which is NOT guaranteed to be an RMS-writable directory in the
     * booted image: measured against the PARTS 0.2 demo, a SYSTEM session's
     * sys$create in its own [SYSMGR] home returns RMS/EACCES (only [SYSTMP]/
     * [USERS] are mastered writable), so PARTS fell through to a fallback
     * candidate and DIRECTORY SYS$SCRATCH:PARTS.DAT then looked elsewhere.
     * Making per-account login directories RMS-writable is a vmsfs/provision
     * concern outside this login-logicals item (tracked separately). SYS$LOGIN
     * and SYS$LOGIN_DEVICE -- the identity this item is about -- are sourced
     * from SYSUAF above regardless; SYS$SCRATCH stays the system scratch.
     */
    (void)0;
    return st;
}
