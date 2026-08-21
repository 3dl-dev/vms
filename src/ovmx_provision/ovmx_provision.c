/*
 * ovmx_provision.c - PROVISION.EXE, the OVMX startup process
 *
 * ============================================================================
 * WHY THIS IMAGE EXISTS (vms-9b7)
 * ============================================================================
 * Three pieces of VMS system management used to live inside PID 1
 * (src/ovmx_init/ovmx_init.c), for no reason except that PID 1 was the only
 * thing running at the time they were written:
 *
 *   provision_sysuaf_users()      create and chown each account's home
 *                                 directory -- ACCOUNT PROVISIONING, which on
 *                                 OpenVMS is AUTHORIZE's job driven from a DCL
 *                                 procedure;
 *   provision_ownership()         give the system tree to SYSTEM's UIC;
 *   establish_system_identity()   read SYSUAF's SYSTEM record and ask the
 *                                 executive to stamp it.
 *
 * All three read SYS$SYSTEM:SYSUAF.DAT. PID 1 is statically linked (it must
 * run before IMGACT.EXE exists in the process tree), so rather than link a
 * parser it grew TWO of its own -- sysuaf_split() and sysuaf_field(), both
 * with char line[512]. Those were two of FIVE independent parsers of one file
 * format, carrying THREE different line limits (512, 512, 1024, 512, 1024) and
 * TWO disagreeing writer format strings.
 *
 * MEASURED, on real QEMU boots of the unfixed tree, each with a control: a
 * SYSTEM row whose sixth field separator falls past byte 511 is read by the
 * 512-byte parsers as a five-field record. PID 1 then reported
 *
 *     %OVMX-F-EXECINIT, no SYSTEM record in SYS$SYSTEM:SYSUAF.DAT
 *
 * and powered the machine off -- while every 1024-byte reader in the tree
 * accepted the same row without complaint. The halt is FATAL BY DESIGN
 * (Rule 10: there is no VMS in which the system process has no identity, so
 * the condition is made unreachable rather than handled), which is exactly why
 * five parsers of one format is intolerable: writer and reader disagree, and
 * the unreachable state is entered through the side door.
 *
 * THE FIX WAS NOT A BIGGER BUFFER. PID 1 needs SYSUAF for nothing. It is a
 * bootstrap: mounts, the executive, the device table, an install-time file
 * copy, and the logical names that make SYS$SYSTEM: resolve. After that a
 * normal image can run, and everything above is normal-image work.
 *
 * ============================================================================
 * WHY THIS IMAGE IS THE STARTUP PROCESS, AND NOT SOMETHING STARTUP.COM CALLS
 * ============================================================================
 * vms_kif_establish_system() (formerly vms_kif_setident()'s job, see
 * vms-a17e above) stamps THE CALLING PROCESS. An identity established by a
 * short-lived helper invoked from a DCL procedure would evaporate when the
 * helper exited, leaving nothing on the node holding SYSTEM.
 *
 * So PID 1 execs THIS image where it used to exec DCL.EXE, and this image
 * execs DCL.EXE on SYS$MANAGER:STARTUP.COM when it is done. exec(2) preserves
 * the process, so the executive's row -- SYSTEM, [1,4], the SYSTEM privilege
 * mask -- carries into the DCL that runs STARTUP.COM and SYSTARTUP_VMS.COM.
 * That is LOGINOUT's shape one step further corrected (ask the executive to
 * construct the identity, exec the CLI -- there is no record left for this
 * image to read first), run for the startup process, and it is what
 * OpenVMS does: STARTUP runs under username SYSTEM.
 *
 * It also answers the ordering question the design left open -- "the identity
 * must be established before STARTUP.COM can act as SYSTEM" -- in the only way
 * that needs no new mechanism: it happens in the same process, immediately
 * before.
 *
 * PID 1 KEEPS ITS OWN IDENTITY OUT OF THIS. Whatever the executive derived for
 * PID 1 from its real credentials at registration is what PID 1 holds. It
 * declares nothing about itself, which is the whole of Rule 11.
 *
 * ============================================================================
 * ONE READER -- AND, AS OF vms-a17e, ONE FEWER JOB FOR IT TO DO
 * ============================================================================
 * This image no longer calls sysuaf_lookup() at all. The SYSTEM identity
 * this file used to read out of SYSUAF's SYSTEM record now comes from the
 * executive's own constants (vms_kif_establish_system(), see the comment at
 * its call site in main() below) -- EXEC_INIT constructs the system
 * process's identity on OpenVMS, and vms.ko now does the OVMX equivalent at
 * module init, so this image has nothing left to read for it.
 *
 * The one SYSUAF read that remains here is provision_home_directories()'s
 * walk of every account for home-directory provisioning -- a distinct
 * concern (VMS's own account provisioning, not identity establishment) that
 * goes through the binary $UAFDEF engine (ovmx_sysuaf_enum) every other reader
 * in the tree uses now. There is still no second parser of SYSUAF anywhere in
 * the boot path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sysuaf.h"
#include "ovmx_layout.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "vms_kif.h"
/* vms-aac: PROVISION seeds the on-volume OPERATOR.LOG from the kmsg ring buffer
 * over the ACP -- PID 1's bridge cannot (no RMS in the static PID-1 image). */
#include "opcom_kmsg.h"
#include "rms_textfile.h"
/* ovmx_boot_power_off(): the boot-plumbing substrate seam (vms-28f) PID 1
 * already uses for the exact same power-off-on-fatal-condition need --
 * Linux: sync(); reboot(RB_POWER_OFF). NetBSD: sync();
 * reboot(RB_HALT | RB_POWERDOWN, NULL). PROVISION.EXE reuses the SAME
 * backend objects ovmx_init links (src/ovmx_init/ovmx_boot_{linux,netbsd}.c,
 * selected by src/ovmx_provision/CMakeLists.txt the same way
 * src/ovmx_init/CMakeLists.txt already does) rather than calling reboot(2)
 * directly a second time with a second, drifting set of flags (vms-5d1,
 * epic vms-8e8: netbsd-vax is a boot-required image too, and NetBSD's
 * reboot(2) has a different signature and flag names than Linux's). */
#include "ovmx_boot.h"

#if defined(__linux__)
/*
 * ACCOUNT/SYSTEM-TREE PROVISIONING OVER THE FILES-11 ODS-2 ACP (vms-4ac).
 *
 * Post-flip (epic vms-208) there is no /vms POSIX passthrough on the runtime
 * path, so the lchown()/mkdir() this file used to do -- which now hit a
 * non-existent path and were silently swallowed (ENOENT) -- set NOTHING on the
 * genuine ODS-2 volume. Ownership and directory creation are done the VMS way
 * now: $ASSIGN a channel to the mounted volume and drive IO$_MODIFY (set the
 * file header's fh2_fileowner) / IO$_CREATE (a new directory) over /dev/vms --
 * the SAME executive ACP acp_check_access() reads back. Fail-honest (Rule 9 /
 * INV-6): no mounted volume / no /dev/vms => the real SS$_ status and a warning,
 * never a POSIX substitute.
 *
 * struct vms_acp_* and the VMS_ACP_* constants arrive via vms_kif.h ->
 * vms_ioctl.h -> vms_acp.h. */
#include "ssdef.h"
#include "vmsfs/ods2.h"      /* ODS2_FH2_M_DIRECTORY (the CREATE dir attr) */
/*
 * rms_acp_resolve_did(): the shared ACP directory-FID resolver ($OPEN/$CREATE/
 * $SEARCH all use it, rms_internal.h). That header is private to libvmsrms and
 * is not on this image's include path, so the prototype is declared here; the
 * symbol is exported by libvmsrms, which this image links (CMakeLists.txt).
 */
uint32_t rms_acp_resolve_did(uint32_t chan, const char *dirpath,
                             uint16_t *dn, uint16_t *ds,
                             uint8_t *dr, uint8_t *dx);
#endif /* __linux__ */

/*
 * Translate a VMS filespec to a Linux path. Same wrapper PID 1 uses, for the
 * same reason: every path in this file is a VMS filespec, translated at point
 * of use.
 */
static const char *vms_to_linux(const char *vms_spec, char *buf, size_t bufsz)
{
    if (vmsfs_to_linux_path(vms_spec, buf, bufsz) == 1)
        return buf;
    snprintf(buf, bufsz, "%s", vms_spec);
    return buf;
}

/*
 * FAIL-STOP, and deliberately so.
 *
 * This carries the SAME facility, the SAME two lines and the SAME
 * power-off-instead-of-exit behaviour PID 1's ovmx_exec_halt() had when the
 * original check lived there. Two conditions reach it now (vms-a17e split
 * what used to be one): no SYSTEM account anywhere in SYSUAF (nobody could
 * ever authenticate as SYSTEM again -- see provision_home_directories()),
 * and the executive refusing to construct or read back the system process's
 * identity (a condition that should be unreachable given CAP_SYS_ADMIN, but
 * is checked rather than assumed). Rule 10 is explicit that a condition VMS
 * never faces gets made unreachable rather than handled, and that there is
 * no plausible-looking default to pick.
 *
 * The facility is OVMX, not a VMS one, because the event is OVMX's -- the
 * message names an OVMX file layout and an OVMX bootstrap, and inventing a
 * VMS-looking message for a state VMS is never in is the exact thing Rule 10
 * forbids.
 *
 * Power off rather than exit: the startup process exiting would leave PID 1
 * looping on a login prompt for a system that has no identity. Powering off
 * ends the boot with the diagnostic still on the console. If reboot(2) is
 * unavailable, exit nonzero -- PID 1's run_startup() treats a nonzero exit
 * from this image as fatal for the same reason.
 */
static void provision_halt(const char *what, const char *detail)
{
    fprintf(stderr, "%%OVMX-F-EXECINIT, %s\n", what);
    if (detail)
        fprintf(stderr, "%%OVMX-I-EXECINIT, %s\n", detail);
    fflush(NULL);
    sync();
    ovmx_boot_power_off();
    _exit(1);
}

/* ------------------------------------------------------------------ */
/* Ownership                                                          */
/* ------------------------------------------------------------------ */

/*
 * Give the system root ([SYS0] and everything under it) to the UIC that OWNS
 * the OpenVMS system tree: the SYSTEM account's; and give each account its own
 * home directory, owned by that account. On the product runtime (__linux__)
 * this is done over the genuine Files-11 ODS-2 ACP -- IO$_MODIFY writes the
 * file header's fh2_fileowner, the same field acp_check_access() reads back
 * (vms-4ac). It used to be POSIX lchown()/mkdir() on the /vms passthrough,
 * which the atomic flip (epic vms-208) retired: the calls hit a non-existent
 * path, returned ENOENT, and were swallowed -- so NOTHING was owned and, once
 * a session genuinely became UIC [1,4] or a plain user's UIC, there was
 * nothing on the volume that account owned to write into (%RMS-E-CRE).
 *
 * PINNED TO THE ORACLE, not chosen (CLAUDE.md Rule 10). Measured on VAX2 of
 * the reference lab, OpenVMS VAX V7.3, 30-JUL-2026 -- verbatim transcripts in
 * docs/oracle/vax73-privileges.md S7:
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
 * write -- exactly the pair of facts OVMX has to reproduce: the SYSTEM account
 * can create and delete in SYS$SYSTEM: and SYS$MANAGER:, and an ordinary user
 * cannot.
 *
 * WHICH UIC IS NOT HARDCODED HERE -- it is `info.uic`, read back from the
 * executive after vms_kif_establish_system() (vms-a17e), not from SYSUAF. It
 * IS hardcoded one level up, in vms.ko's VMS_SYSTEM_UIC (vms_internal.h) --
 * but that constant and SYSUAF's SYSTEM row are oracle-pinned to the SAME fact
 * ([1,4], docs/oracle/vax73-authorize-privilege.md), not read from one
 * another, so they cannot drift apart the way two independent parsers of one
 * file could.
 *
 * WHY IT RUNS ON EVERY BOOT, not just at install: PID 1's
 * provision_seed_files() adds files to the tree after the install step is
 * skipped, and a file seeded by root that SYSTEM cannot write is the same
 * regression in a smaller box.
 *
 * DISCLOSED DIVERGENCE (not a handler, a platform limit): VMS grants the
 * SYSTEM protection category to every UIC whose group is <= MAXSYSGROUP
 * (measured 8, see S7). acp_check_access() already reproduces that (group <=
 * ACP_MAXSYSGROUP), so a system-group account reaches the system tree through
 * the SYSTEM protection field; the owner set here is what makes the OWNER
 * category apply to [1,4] specifically and, for a plain user, to that user's
 * own login directory.
 */
#if defined(__linux__)

/* One directory entry captured during an ACP wildcard enumeration. */
struct acp_ent {
    uint16_t fid_num, fid_seq;
    uint8_t  fid_rvn, fid_nmx;
    int      is_dir;
};

/* Set one file/dir's ODS-2 fh2_fileowner over the executive ACP (IO$_MODIFY by
 * FID). Fail-honest: a denied/absent header yields its SS$_ and a warning. */
static void own_object_acp(uint32_t chan,
                           uint16_t fid_num, uint16_t fid_seq,
                           uint8_t fid_rvn, uint8_t fid_nmx,
                           uint32_t uic_group, uint32_t uic_member,
                           const char *label)
{
    struct vms_acp_fileop_args fop;
    memset(&fop, 0, sizeof(fop));
    fop.chan     = chan;
    fop.func     = VMS_ACP_FOP_MODIFY;
    fop.fidmode  = 1;                     /* by FID: no name/DID needed */
    fop.fid_num  = fid_num; fop.fid_seq = fid_seq;
    fop.fid_rvn  = fid_rvn; fop.fid_nmx = fid_nmx;
    fop.attr_ctl = VMS_ACP_ATTR_OWNER;    /* write fh2_fileowner only */
    fop.attr.uic_group  = (uint16_t)uic_group;
    fop.attr.uic_member = (uint16_t)uic_member;

    uint32_t st = vms_kif_acp_fileop(&fop);
    if (!(st & 1))
        fprintf(stderr, "%%OVMX-W-OWNER, cannot set owner of %s: SS$ %u\n",
                label ? label : "<file>", (unsigned)st);
}

/*
 * Recursively give a directory's CONTENTS -- and, depth-first, its
 * subdirectories -- to a UIC over the ACP (the directory's OWN header is set by
 * the caller). Each directory is enumerated FULLY into an array first: the
 * executive holds the wildcard cursor per-channel, so a nested search would
 * clobber the parent's context if opened before the parent's search finishes.
 */
static void own_dir_contents_acp(uint32_t chan,
                                 uint16_t did_num, uint16_t did_seq,
                                 uint8_t did_rvn, uint8_t did_nmx,
                                 uint32_t grp, uint32_t mem, int depth)
{
    if (depth > 32)                        /* guard against a pathological loop */
        return;

    struct acp_ent *ents = NULL;
    size_t n = 0, cap = 0;
    struct vms_acp_acpcontrol_args a;
    int first = 1;

    for (;;) {
        memset(&a, 0, sizeof(a));
        a.chan = chan;
        a.func = VMS_ACP_CTL_SEARCH;
        if (first) {
            a.did_num = did_num; a.did_seq = did_seq;
            a.did_rvn = did_rvn; a.did_nmx = did_nmx;
            a.wcc_reset = 1;               /* (re)open the wildcard context */
            strncpy(a.pattern, "*.*;*", VMS_ACP_NAME_SIZE - 1);
            first = 0;
        } else {
            a.wcc_reset = 0;               /* continue */
        }
        if (vms_kif_acp_acpcontrol(&a) != SS$_NORMAL)   /* SS$_NOMOREFILES ends */
            break;

        /* Belt-and-braces: skip any child whose FID equals this DID (ODS-2
         * directories carry no "." self-entry, but the MFD's own alias does). */
        if (a.fid_num == did_num && a.fid_nmx == did_nmx)
            continue;

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            struct acp_ent *ne = realloc(ents, ncap * sizeof(*ne));
            if (!ne) break;                /* OOM: own what we captured */
            ents = ne; cap = ncap;
        }
        ents[n].fid_num = a.fid_num; ents[n].fid_seq = a.fid_seq;
        ents[n].fid_rvn = a.fid_rvn; ents[n].fid_nmx = a.fid_nmx;
        ents[n].is_dir  = (strstr(a.resnam, ".DIR;") != NULL);
        n++;
    }

    for (size_t i = 0; i < n; i++) {
        own_object_acp(chan, ents[i].fid_num, ents[i].fid_seq,
                       ents[i].fid_rvn, ents[i].fid_nmx, grp, mem, NULL);
        if (ents[i].is_dir)
            own_dir_contents_acp(chan, ents[i].fid_num, ents[i].fid_seq,
                                 ents[i].fid_rvn, ents[i].fid_nmx,
                                 grp, mem, depth + 1);
    }
    free(ents);
}

/*
 * Resolve `dirpath` (dotted, no brackets) on `chan` and give that directory --
 * its own header AND everything beneath it -- to [grp,mem] over the ACP.
 * Returns 1 if the directory resolved (and was re-owned), 0 otherwise.
 */
static int own_spec_tree_acp(uint32_t chan, const char *dirpath,
                             uint32_t grp, uint32_t mem, const char *label)
{
    uint16_t dn, ds; uint8_t dr, dx;
    if (!(rms_acp_resolve_did(chan, dirpath, &dn, &ds, &dr, &dx) & 1))
        return 0;
    own_object_acp(chan, dn, ds, dr, dx, grp, mem, label);    /* the dir header */
    own_dir_contents_acp(chan, dn, ds, dr, dx, grp, mem, 0);  /* its contents   */
    return 1;
}

/* "DEV:[A.B.C]NAME.TYP;VER" (or "[A.B.C]") -> device (keeping ':') and dotted
 * dirpath ("A.B.C", no brackets). Either output may come back empty. */
static void spec_split_dir(const char *spec, char *dev, size_t devsz,
                           char *dir, size_t dirsz)
{
    dev[0] = '\0'; dir[0] = '\0';
    const char *lb = strchr(spec, '[');
    if (!lb) lb = strchr(spec, '<');
    const char *colon = strchr(spec, ':');
    if (colon && (!lb || colon < lb)) {
        size_t dl = (size_t)(colon - spec) + 1;      /* keep the ':' */
        if (dl < devsz) { memcpy(dev, spec, dl); dev[dl] = '\0'; }
    }
    if (lb) {
        const char *rb = (lb[0] == '[') ? strchr(lb, ']') : strchr(lb, '>');
        if (rb && rb > lb + 1) {
            size_t dl = (size_t)(rb - lb - 1);
            if (dl >= dirsz) dl = dirsz - 1;
            memcpy(dir, lb + 1, dl); dir[dl] = '\0';
        }
    }
}

static void provision_ownership(uint32_t sys_grp, uint32_t sys_mem)
{
    uint32_t chan = 0;
    const char *dev = SYSDISK_DEVICE ":";            /* "DKA0:" */
    uint32_t st = vms_kif_acp_assign(dev, &chan);
    if (!(st & 1)) {
        fprintf(stderr, "%%OVMX-W-OWNER, cannot $ASSIGN %s for system-tree "
                "ownership: SS$ %u\n", dev, (unsigned)st);
        return;
    }
    /* [SYS0] -- the top of the OpenVMS system tree (parent of VMS_SYSROOT
     * [SYS0.SYSCOMMON]); its own header lives in [000000]. Own it, then walk
     * everything beneath -- SYS$SYSTEM: and SYS$MANAGER: are under here. */
    if (!own_spec_tree_acp(chan, "SYS0", sys_grp, sys_mem, "[000000]SYS0.DIR"))
        fprintf(stderr,
                "%%OVMX-W-OWNER, system tree [SYS0] did not resolve over the "
                "ACP\n");
    vms_kif_dassgn(chan);
}

/*
 * Give one account its login directory, owned by that account, over the ACP.
 * If the directory already exists it is (re)owned; if it does not, it is
 * CREATED as a directory in its parent, owned by the account (the ODS-2 class
 * default protection for a directory, 0xBA00, leaves the OWNER RWED -- so the
 * account can create files in its own login directory). This is VMS's own
 * account-provisioning step (AUTHORIZE ADD + CREATE/DIRECTORY/OWNER=[g,m]),
 * done here on the startup path where account provisioning belongs.
 */
static void provision_home(uint32_t uic_group, uint32_t uic_member,
                           const char *home_spec)
{
    /* Resolve any concealed/rooted device logical (SYS$SYSDEVICE:, SYS$LOGIN,
     * ...) to fully-composed physical ODS-2 candidate specs, exactly as RMS
     * $CREATE does. The account's default directory is where it must be able to
     * create files. */
    char cands[4][VMSFS_MAX_FILESPEC];
    int ncand = vmsfs_compose_ods2_candidates(home_spec, cands, 4);
    if (ncand <= 0) {
        snprintf(cands[0], sizeof(cands[0]), "%s", home_spec);
        ncand = 1;
    }

    for (int ci = 0; ci < ncand; ci++) {
        char dev[16], dir[256];
        spec_split_dir(cands[ci], dev, sizeof(dev), dir, sizeof(dir));
        if (dir[0] == '\0')
            continue;
        if (dev[0] == '\0')
            snprintf(dev, sizeof(dev), "%s", SYSDISK_DEVICE ":");

        uint32_t chan = 0;
        if (!(vms_kif_acp_assign(dev, &chan) & 1))
            continue;                          /* try the next candidate */

        /* Present already? -> just (re)own it (and anything under it). */
        if (own_spec_tree_acp(chan, dir, uic_group, uic_member, home_spec)) {
            vms_kif_dassgn(chan);
            return;
        }

        /* Missing -> CREATE it as a directory in its parent, owned by the
         * account. Split "A.B.LEAF" into parent "A.B" + leaf "LEAF" (leaf kept
         * as a pointer into dir; its length is bounded below by the ODS-2 name
         * field the CREATE writes). */
        char parent[256];
        const char *leaf;
        char *lastdot = strrchr(dir, '.');
        if (lastdot) {
            size_t pl = (size_t)(lastdot - dir);
            if (pl >= sizeof(parent)) pl = sizeof(parent) - 1;
            memcpy(parent, dir, pl); parent[pl] = '\0';
            leaf = lastdot + 1;
        } else {
            parent[0] = '\0';                  /* the leaf sits in the MFD */
            leaf = dir;
        }

        uint16_t dn, ds; uint8_t dr, dx;
        if (rms_acp_resolve_did(chan, parent, &dn, &ds, &dr, &dx) & 1) {
            struct vms_acp_fileop_args fop;
            char name[VMS_ACP_NAME_SIZE];
            int nn = snprintf(name, sizeof(name), "%s.DIR", leaf);
            if (nn < 0 || nn >= (int)sizeof(name)) {   /* leaf too long -> invalid */
                fprintf(stderr,
                        "%%OVMX-W-OWNER, home directory name too long: %s\n",
                        home_spec);
                vms_kif_dassgn(chan);
                return;
            }
            memset(&fop, 0, sizeof(fop));
            fop.chan      = chan;
            fop.func      = VMS_ACP_FOP_CREATE;
            fop.modifiers = VMS_ACP_M_CREATE;         /* enter in the parent DID */
            fop.did_num = dn; fop.did_seq = ds; fop.did_rvn = dr; fop.did_nmx = dx;
            fop.version  = 1;                          /* directories are ;1 */
            fop.attr_ctl = VMS_ACP_ATTR_OWNER;        /* owner = the account */
            fop.attr.filechar   = ODS2_FH2_M_DIRECTORY;
            fop.attr.uic_group  = (uint16_t)uic_group;
            fop.attr.uic_member = (uint16_t)uic_member;
            strncpy(fop.name, name, VMS_ACP_NAME_SIZE - 1);

            uint32_t st = vms_kif_acp_fileop(&fop);
            if (!(st & 1))
                fprintf(stderr,
                        "%%OVMX-W-OWNER, cannot create home %s: SS$ %u\n",
                        home_spec, (unsigned)st);
            vms_kif_dassgn(chan);
            return;
        }
        vms_kif_dassgn(chan);
    }
    fprintf(stderr,
            "%%OVMX-W-OWNER, home directory %s did not resolve over the ACP "
            "(parent missing?)\n", home_spec);
}

#else  /* !__linux__ : the netbsd-vax standalone cross keeps the POSIX /vms
        * passthrough path until the VAX mount retires with it (vms-d5d). */

/* Give one filesystem object to a UIC, without following symlinks (lchown, not
 * chown: the install copy preserves symlinks and re-owning one must not re-own
 * its target). */
static void own_object(const char *path, uint32_t uic_group, uint32_t uic_member)
{
    if (lchown(path, (uid_t)uic_member, (gid_t)uic_group) != 0 &&
        errno != ENOENT)
        fprintf(stderr, "%%OVMX-W-OWNER, cannot set owner of %s: %s\n",
                path, strerror(errno));
}

/* Give a directory and everything beneath it to a UIC. */
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

static void provision_ownership(uint32_t sys_grp, uint32_t sys_mem)
{
    char path[512];
    vms_to_linux(VMS_SYSROOT, path, sizeof(path));

    /* [SYS0] -- the parent of VMS_SYSROOT ([SYS0.SYSCOMMON]) -- and
     * everything beneath it. */
    char *last_slash = strrchr(path, '/');
    if (last_slash && last_slash != path)
        *last_slash = '\0';
    own_tree(path, sys_grp, sys_mem);
}

static void provision_home(uint32_t uic_group, uint32_t uic_member,
                           const char *home_spec)
{
    char home_linux[512];
    if (strchr(home_spec, '[') || strchr(home_spec, ':'))
        vms_to_linux(home_spec, home_linux, sizeof(home_linux));
    else
        snprintf(home_linux, sizeof(home_linux), "%s", home_spec);

    mkdir(home_linux, 0755);
    own_object(home_linux, uic_group, uic_member);
}

#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* Account home directories                                            */
/* ------------------------------------------------------------------ */

/*
 * Create each account's home directory, OWNED BY THAT ACCOUNT'S UIC.
 *
 * WHY THE OWNERSHIP IS PART OF PROVISIONING AND NOT PART OF LOGIN (vms-2b8
 * round 7). Until LOGINOUT started dropping to the authenticated user's
 * credentials, every VMS session on OVMX ran as Linux root, so it did not
 * matter who owned anything: root passes every DAC check. The moment a session
 * really becomes UIC [1,4], a tree installed as root:root is a tree that VMS's
 * own SYSTEM account cannot write -- and that is not a VMS state.
 *
 * On OpenVMS the account's directory is created BY THE ACCOUNT PROVISIONING
 * (the System Manager's Manual add-user procedure is AUTHORIZE ADD followed by
 * CREATE/DIRECTORY .../OWNER=[g,m]); LOGINOUT does not create it and does not
 * re-own it. OVMX does the same, here -- in an ordinary image on the startup
 * path, which is where account provisioning belongs, rather than in PID 1.
 *
 * ONE READER (vms-d92 atomic flip). This walk goes through ovmx_sysuaf_enum()
 * -- the binary $UAFDEF indexed engine over the ACP -- the same reader the rest
 * of the tree uses now; there is no ASCII parse and no /vms fopen.
 *
 * ALSO THE SYSTEM-ACCOUNT CONTINUITY CHECK (vms-a17e). Identity
 * establishment no longer reads SYSUAF -- vms_kif_establish_system()
 * constructs SYSTEM's identity as an executive constant regardless of
 * what SYSUAF contains -- but "SYSUAF has no SYSTEM account at all" is
 * still a condition worth a fail-stop for a reason that has nothing to do
 * with identity: no session could ever authenticate as SYSTEM again.
 * Rather than add a SECOND SYSUAF read to check for that, this walk
 * (which runs anyway, for home-directory provisioning) reports whether it
 * saw one. Returns 1 if a SYSTEM row was seen, 0 otherwise -- including
 * when the file could not be opened at all, which is the same "no SYSTEM
 * account" condition by a different route.
 */
/*
 * BINARY SYSUAF ENUMERATION (vms-d92 atomic flip). PROVISION reads every
 * account from the genuine $UAFDEF indexed file through the binary engine
 * (ovmx_sysuaf_enum, src/vmsrms/sysuaf_live.c over the ACP) -- no fopen on a
 * /vms passthrough, no ASCII parse. PROVISION links LIBVMSRMS and runs at boot
 * with the executive present, so the enumeration rides the ODS-2 ACP.
 */
typedef int (*ovmx_sysuaf_enum_cb)(const sysuaf_rms_record_t *rec, void *arg);
uint32_t ovmx_sysuaf_enum(ovmx_sysuaf_enum_cb cb, void *arg);

/*
 * The SYSUAF enumeration only READS here -- it records whether a SYSTEM account
 * exists (the continuity fail-stop) and COLLECTS each account's [uic, home] so
 * the actual directory creation + ownership can run LATER, after
 * vms_kif_establish_system(), under the deliberate SYSTEM identity that holds
 * the privilege to create in [USERS] and stamp an arbitrary owner (vms-4ac).
 * Doing the writes inside this read callback would run them before the identity
 * is established.
 */
#define PROV_MAX_HOMES 64
struct prov_home {
    uint32_t uic_group, uic_member;
    char     spec[256];
};
struct prov_enum {
    int              saw_system;
    int              nhomes;
    struct prov_home homes[PROV_MAX_HOMES];
};

static int prov_home_cb(const sysuaf_rms_record_t *raw, void *arg)
{
    struct prov_enum *pe = (struct prov_enum *)arg;
    sysuaf_record_t rec;
    sysuaf_raw_to_view(raw, &rec);

    if (strcmp(rec.username, "SYSTEM") == 0)
        pe->saw_system = 1;
    if (rec.default_dir[0] == '\0')
        return 0;

    if (pe->nhomes < PROV_MAX_HOMES) {
        struct prov_home *h = &pe->homes[pe->nhomes++];
        h->uic_group  = rec.uic_group;
        h->uic_member = rec.uic_member;
        snprintf(h->spec, sizeof(h->spec), "%s", rec.default_dir);
    }
    return 0;
}

/*
 * Enumerate SYSUAF, filling `pe` with the collected homes, and return whether a
 * SYSTEM account was seen. A failed enumeration (no file / no executive) counts
 * as "no SYSTEM account" -- the same fail-stop condition by a different route.
 */
static int provision_scan_accounts(struct prov_enum *pe)
{
    memset(pe, 0, sizeof(*pe));
    uint32_t st = ovmx_sysuaf_enum(prov_home_cb, pe);
    if (!(st & 1))
        return 0;
    return pe->saw_system;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

#ifndef __NetBSD__
/*
 * emit one already-classified kmsg line into the genuine Files-11
 * SYS$MANAGER:OPERATOR.LOG over the ACP, via RMS $PUT-at-EOF (vms-aac). The
 * first append $CREATEs the log. Fail-honest: rms_textfile_append_line returns
 * -1 with no mounted ACP volume / no /dev/vms and no line is lost to a POSIX
 * substitute -- there just is no operator log to seed then (Rule 9 / INV-6).
 */
static void provision_oplog_emit(const char *line)
{
    (void)rms_textfile_append_line(VMS_OPERATOR_LOG, line);
}
#endif

int main(void)
{
    /* Bootstrap the VMS namespace so filespecs translate. Same two calls
     * every OVMX image makes; PID 1 has already mounted the disk. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

#ifndef __NetBSD__
    /*
     * SEED THE OPERATOR LOG (vms-aac, epic vms-208 atomic flip). PID 1's
     * /dev/kmsg bridge routed vms.ko/vmsfs.ko's boot lifecycle events, but PID 1
     * is static and has no RMS to write the on-volume OPERATOR.LOG. We do -- and
     * the SYS$DISK is mounted (PID 1 mounted it before exec'ing us) and the
     * namespace is up (the two calls just above), so this replays the ring
     * buffer's boot records into SYS$MANAGER:OPERATOR.LOG over the ACP now,
     * race-free, before STARTUP.COM runs. sys$sndopr appends live records to the
     * same file thereafter.
     */
    opcom_kmsg_drain_ringbuffer(provision_oplog_emit);
#endif

    /*
     * SCAN SYSUAF AND THE ONE REMAINING SYSUAF READ, FIRST (vms-a17e). The
     * account scan is unrelated to identity -- it collects every account's home
     * directory for provisioning below -- but its walk is also where "does
     * SYSUAF have a SYSTEM account at all" gets answered, so the fail-stop below
     * rides that read instead of adding a second one (see prov_home_cb).
     * Deliberately BEFORE identity establishment: a SYSUAF with no SYSTEM
     * account must halt before anything prints "system identity ... established",
     * exactly as it did when this same check lived in the now-deleted
     * SYSUAF-for-identity read (#278). The actual home creation + ownership is
     * DEFERRED to after establishment (vms-4ac), so it runs as SYSTEM.
     */
    static struct prov_enum accounts;
    if (!provision_scan_accounts(&accounts))
        provision_halt("no SYSTEM account in SYS$SYSTEM:SYSUAF.DAT",
                       "no session could ever authenticate as SYSTEM");

    /*
     * ASK THE EXECUTIVE TO CONSTRUCT THE IDENTITY (vms-a17e). NO SYSUAF READ
     * PRECEDES THIS. SYSTEM/[1,4]/ALL are no longer values this process
     * reads out of SYS$SYSTEM:SYSUAF.DAT and hands to the executive -- they
     * are constants vms.ko owns from module init (VMS_SYSTEM_UIC /
     * VMS_PRV_M_SYSTEM_ALL, src/kernel/vms_internal.h), constructed the same
     * way OPA0: is: by the executive itself, before any process asks. This
     * is the OpenVMS shape restored -- EXEC_INIT constructs the system
     * process's identity; LOGINOUT (tools/vms_login.c) is SYSUAF's FIRST
     * reader, not this image.
     *
     * The refusal path is unchanged: vms_kif_establish_system() can still
     * say no (SS$_NOPRIV, if this process somehow lacks CAP_SYS_ADMIN),
     * which is still the difference between an identity and a claim
     * (Rule 11) -- there is just nothing left for THIS process to claim.
     */
    uint32_t st = vms_kif_establish_system();
    if (!(st & 1)) {
        char detail[64];
        snprintf(detail, sizeof(detail), "SS$ status %u", (unsigned)st);
        provision_halt("the executive refused to construct the system process's identity",
                       detail);
    }

    /* Read the row back out of the executive and report THAT, never what we
     * asked for. The values printed are the executive's verdict. */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    st = vms_kif_getjpi_self(&info);
    if (!(st & 1))
        provision_halt("the executive holds no row for the system process",
                       "identity was stamped but cannot be read back");

    printf("%%OVMX-I-EXEC, system identity %s [%o,%o] established by the executive\n",
           info.username, (unsigned)((info.uic >> 16) & 0xFFFFu),
           (unsigned)(info.uic & 0xFFFFu));
    fflush(stdout);

    /*
     * PROVISION NOW, AS SYSTEM (vms-4ac). Both steps write ODS-2 ownership over
     * the ACP, and both run here -- after vms_kif_establish_system() -- so they
     * carry the SYSTEM identity (and its BYPASS) needed to create in [USERS] and
     * stamp an arbitrary owner. Home directories FIRST, then the system tree
     * LAST: the system-tree walk is the last writer of ownership for [SYS0], so
     * an account whose home sits under [SYS0] (SYSTEM's [SYSMGR]) ends up owned
     * by SYSTEM regardless of enumeration order, while [USERS.*] homes keep the
     * per-account owner set here (the tree walk never descends into [USERS]).
     */
    for (int i = 0; i < accounts.nhomes; i++)
        provision_home(accounts.homes[i].uic_group,
                       accounts.homes[i].uic_member,
                       accounts.homes[i].spec);

    provision_ownership(info.uic >> 16, info.uic & 0xFFFFu);

    /*
     * BECOME THE CLI. exec(2), not fork(2): the process is preserved, so the
     * executive row just written -- SYSTEM, its UIC, its authorized mask --
     * is the identity STARTUP.COM and SYSTARTUP_VMS.COM run under. A fork
     * would leave the child to register itself from scratch and the identity
     * would not follow.
     */
    char startup_path[512], dcl_path[512];
    vms_to_linux(VMS_STARTUP_PATH, startup_path, sizeof(startup_path));
    vms_to_linux(VMS_DCL_PATH, dcl_path, sizeof(dcl_path));

    /* ATOMIC FLIP (vms-5f0): execve the DCL.EXE copy PID 1 staged off the
     * ODS-2 volume over the ACP into OVMX_BOOT_STAGE_DIR -- the /vms POSIX
     * path no longer exists for the kernel to map. Self-guarding: use the
     * staged copy only if it is present, so a substrate that did not stage
     * (NetBSD-vax, vms-d5d) keeps the original path. */
    {
        char staged[512];
        if (ovmx_boot_stage_exec_path(dcl_path, staged, sizeof(staged)) &&
            access(staged, X_OK) == 0)
            snprintf(dcl_path, sizeof(dcl_path), "%s", staged);
    }

    /* STARTUP.COM presence is DCL's to resolve. With the /vms passthrough
     * retired it is read through the executive ACP (RMS/$QIO), not a POSIX
     * stat here -- a stat() of the retired /vms path would spuriously report
     * "no site startup" on a volume that in fact carries it. DCL reports an
     * absent procedure itself. On a substrate that still exposes the POSIX
     * tree the stat is a harmless best-effort warning, not a gate. */
    struct stat st_buf;
    if (stat(startup_path, &st_buf) != 0)
        fprintf(stderr,
                "%%OVMX-I-STARTUP, handing %s to DCL for ACP resolution\n",
                VMS_STARTUP_PATH);

    /* Hand DCL the VMS filespec (not the retired /vms path) so it opens the
     * procedure through its own ACP-routed RMS path. */
    execl(dcl_path, "vmsdcl", VMS_STARTUP_PATH, (char *)NULL);
    fprintf(stderr, "%%OVMX-E-NOIMG, cannot activate %s: %s\n",
            VMS_DCL_PATH, strerror(errno));
    return 1;
}
