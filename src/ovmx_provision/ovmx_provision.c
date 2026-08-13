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
 * goes through the same sysuaf_read_line()/sysuaf_parse_line() pair every
 * other reader in the tree uses. There is still no second parser of SYSUAF
 * anywhere in the boot path.
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
 * Give one filesystem object to a UIC, without following symlinks.
 *
 * lchown() and not chown(): the install copy preserves symlinks (VMS
 * concealed-device and relative-path links can appear in the tree it copies),
 * and re-owning a symlink must not re-own whatever it points at.
 */
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

/*
 * Give the system root ([SYS0] and everything under it) to the UIC that OWNS
 * the OpenVMS system tree: the SYSTEM account's.
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
 * WHICH UIC IS NOT HARDCODED IN THIS FUNCTION -- it is `info.uic`, read back
 * from the executive after vms_kif_establish_system() (vms-a17e), not from
 * SYSUAF. It IS hardcoded one level up, in vms.ko's VMS_SYSTEM_UIC
 * (vms_internal.h) -- but that constant and SYSUAF's SYSTEM row are
 * oracle-pinned to the SAME fact ([1,4], docs/oracle/vax73-authorize-
 * privilege.md), not read from one another, so they cannot drift apart the
 * way two independent parsers of one file could.
 *
 * WHY IT RUNS ON EVERY BOOT, not just at install: PID 1's
 * provision_seed_files() adds files to the tree after the install step is
 * skipped, and a file seeded by root that SYSTEM cannot write is the same
 * regression in a smaller box.
 *
 * DISCLOSED DIVERGENCE (not a handler, a platform limit): VMS grants the
 * SYSTEM protection category to every UIC whose group is <= MAXSYSGROUP
 * (measured 8, see S7). A Linux inode carries exactly one owning group, so
 * OVMX can express "UIC group 1 is the owner's group" but not "UIC groups 1
 * through 8 are all system". Accounts in groups 2..8 would therefore get VMS's
 * GROUP nibble where VMS gives them the SYSTEM one. OVMX's SYSUAF ships no
 * such account, and inventing a second enforcement layer to paper over it
 * would be worse than the gap (Rule 10).
 */
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
 * ONE READ, ONE PARSER. This walk goes through sysuaf_read_line() and
 * sysuaf_parse_line(), the same pair every other reader in the tree uses, so
 * an over-length row is REPORTED here and not silently downgraded into a
 * different account (vms-9b7).
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
static int provision_home_directories(void)
{
    char sysuaf_path[512];
    vms_to_linux(VMS_SYSUAF_PATH, sysuaf_path, sizeof(sysuaf_path));
    FILE *fp = fopen(sysuaf_path, "r");
    if (!fp)
        return 0;

    int saw_system = 0;
    char line[SYSUAF_LINE_MAX];
    int too_long = 0;
    while (sysuaf_read_line(fp, line, sizeof(line), &too_long)) {
        if (too_long) {
            fprintf(stderr, "%%OVMX-W-RECTOOLONG, SYSUAF record longer than "
                    "%d bytes -- account not provisioned\n",
                    SYSUAF_LINE_MAX - 1);
            continue;
        }

        sysuaf_record_t rec;
        if (sysuaf_parse_line(line, &rec) != 1)
            continue;
        if (strcmp(rec.username, "SYSTEM") == 0)
            saw_system = 1;
        if (rec.default_dir[0] == '\0')
            continue;

        /* default_dir may be a VMS spec (DKA0:[USERS.name]) -- translate. */
        char home_linux[512];
        if (strchr(rec.default_dir, '[') || strchr(rec.default_dir, ':'))
            vms_to_linux(rec.default_dir, home_linux, sizeof(home_linux));
        else
            snprintf(home_linux, sizeof(home_linux), "%s", rec.default_dir);

        mkdir(home_linux, 0755);
        own_object(home_linux, rec.uic_group, rec.uic_member);
    }
    fclose(fp);
    return saw_system;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Bootstrap the VMS namespace so filespecs translate. Same two calls
     * every OVMX image makes; PID 1 has already mounted the disk. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    /*
     * ACCOUNT-PROVISIONING AND THE ONE REMAINING SYSUAF READ, FIRST
     * (vms-a17e). provision_home_directories() is unrelated to identity --
     * it creates every account's home directory -- but its walk is also
     * where "does SYSUAF have a SYSTEM account at all" gets answered, so
     * the fail-stop below rides that read instead of adding a second one
     * (see the function's own comment). Deliberately BEFORE identity
     * establishment: a SYSUAF with no SYSTEM account must halt before
     * anything prints "system identity ... established", exactly as it did
     * when this same check lived in the now-deleted SYSUAF-for-identity
     * read (#278).
     */
    if (!provision_home_directories())
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
     * Ownership LAST among the two provisioning steps: it is the LAST writer
     * of ownership, so that accounts sharing a directory (SYSUAF ships
     * SYSTEM and OPERATOR both defaulted to [SYSMGR]) cannot leave the
     * system tree owned by whichever record was read last.
     * provision_home_directories() already ran, above (vms-a17e moved it
     * earlier to double as the SYSTEM-account continuity check) -- the
     * relative order that matters here is unchanged, home directories then
     * this.
     */
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

    struct stat st_buf;
    if (stat(startup_path, &st_buf) != 0) {
        /*
         * NOT A SILENT SKIP. A missing startup procedure used to be a bare
         * `return` in PID 1 -- the same shape that let VMSSSHD.EXE vanish
         * while the boot banner still announced SSH (see the NOTE ON SERVICES
         * in src/ovmx_init/ovmx_init.c). It is reported, and the boot goes on
         * to the login prompt with the identity established: a system with no
         * SYS$MANAGER:STARTUP.COM is a system with no site startup, which is
         * survivable; a system with no identity is not.
         */
        fprintf(stderr, "%%OVMX-W-NOSTARTUP, %s not found -- no site startup\n",
                VMS_STARTUP_PATH);
        return 0;
    }

    execl(dcl_path, "vmsdcl", startup_path, (char *)NULL);
    fprintf(stderr, "%%OVMX-E-NOIMG, cannot activate %s: %s\n",
            VMS_DCL_PATH, strerror(errno));
    return 1;
}
