/*
 * test_syssvc_libspawn_reg.c - a lib$spawn'd subprocess is EXECUTIVE-REGISTERED
 * under its prcnam, resolvable BY NAME through $GETJPI (vms-e9a, B0).
 *
 * WHAT THIS SUITE GUARDS, AND THE FABRICATION IT DELETES.
 *
 * Before B0 (docs/design-libspawn-ovmx.md §2b) lib$spawn fork()+exec'd the DCL
 * image directly and NEVER entered the child in the executive process table: it
 * accepted `prcnam` and DISCARDED it, so the subprocess had no VMS process ID,
 * no name the executive knew, and was invisible to $GETJPI / SHOW SYSTEM /
 * $DELPRC -- the "reports success while sharing nothing" fabrication class
 * (INV-6). B0 reroutes lib$spawn through the ONE executive-registered creation
 * primitive, $CREPRC (src/libvms/syssvc/sys_process.c), whose child registers
 * under `prcnam` BEFORE it activates the image. This suite proves the fix.
 *
 * THE ASSERTION IS A-CREATES / B-OBSERVES, AND IT CAN FAIL. lib$spawn (in THIS
 * process) creates a subprocess named LSPWN_SUBJECT that runs a real DCL.EXE
 * (WAIT, so it stays alive). A DIFFERENT reader -- $GETJPI resolving BY NAME
 * (vms_kif_getjpi_prcnam) -- must find that name in the executive's table. The
 * name can ONLY be there if lib$spawn's child actually called the executive:
 * the DCL image the child exec'd never sets its own name. Against the pre-B0
 * body (prcnam discarded, nothing registered) the by-name lookup returns
 * SS$_NONEXPR and every positive CHECK below fails -- which is the point.
 *
 * NOT-VACUOUS CONTROL. A name that was NEVER spawned (LSPWN_ABSENT) must NOT
 * resolve. That is asserted first, so a by-name lookup that returned success
 * for everything could not make the positive assertion pass by accident.
 *
 * DCL RESOLUTION. lib$spawn resolves its CLI image as SYS$SYSTEM:DCL.EXE. The
 * qemu harness stages DCL.EXE at /bin/DCL.EXE, so this suite defines SYS$SYSTEM
 * -> /bin (LNM$PROCESS_TABLE, first in LNM$FILE_DEV) exactly as
 * tests/libvms/test_lib_spawn_dcl.c does, and lib$spawn's own resolver then
 * lands on the staged image.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With none -- only the CI
 * negative-control rig, never the product (PID 1 refuses to boot without the
 * executive) -- it runs device_absent_checks() and exits EXIT_SKIP (77), never
 * a fake pass.
 *
 * Doc pins (VSI OpenVMS, public): RTL Library (LIB$) Routines Reference Manual,
 * LIB$SPAWN; System Services Reference, $CREPRC / $GETJPI; DCL Dictionary, WAIT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "clidef.h"
#include "vms_kif.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do {                                 \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }   \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }   \
    } while (0)

/* Unique across the whole initramfs run (one booted executive, one process
 * table shared by every suite). */
#define LSPWN_SUBJECT  "LSPWN_SUBJECT"
#define LSPWN_ABSENT   "LSPWN_ABSENT_X"

/* Build a CLASS_S text descriptor over a C string. */
static struct dsc$descriptor_s dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = s ? (uint16_t)strlen(s) : 0;
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (s && *s) ? (char *)s : NULL;
    return d;
}

/* With no executive, the by-name $GETJPI resolver must not fabricate success,
 * and lib$spawn -- which now needs $CREPRC's registration -- must fail honestly
 * rather than run an unregistered subprocess (design §3d, INV-6). */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    struct vms_procinfo info;
    uint32_t st = vms_kif_getjpi_prcnam(LSPWN_SUBJECT, &info);
    CHECK(!(st & 1),
          "$GETJPI-by-name reports no success with no executive");

    printf("=== test_syssvc_libspawn_reg: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    printf("=== test_syssvc_libspawn_reg (vms-e9a B0: lib$spawn registration) ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    /* PRECONDITION: a staged DCL.EXE lib$spawn can resolve. */
    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  (no /bin/DCL.EXE staged -- cannot drive lib$spawn) SKIP\n");
        return EXIT_SKIP;
    }

    /* Make SYS$SYSTEM:DCL.EXE resolve to the staged /bin/DCL.EXE. */
    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { fprintf(stderr, "no lnm manager\n"); return EXIT_SKIP; }
    uint32_t lst = lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM",
                              "/bin", 0, LNM_MODE_SUPER);
    CHECK(lst & 1, "define SYS$SYSTEM -> /bin (stage DCL.EXE for lib$spawn)");

    char resolved[1024];
    int rr = vmsfs_to_linux_path("SYS$SYSTEM:DCL.EXE", resolved, sizeof(resolved));
    if (!(rr == 1 && access(resolved, X_OK) == 0)) {
        printf("  (SYS$SYSTEM:DCL.EXE did not resolve to an executable: %s) SKIP\n",
               rr == 1 ? resolved : "(unresolved)");
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* CONTROL: the subject name is NOT registered yet, so a by-name $GETJPI
     * must NOT resolve it. This makes the positive assertion below non-vacuous:
     * a resolver that returned success for any name could not pass this. */
    {
        struct vms_procinfo none;
        uint32_t nst = vms_kif_getjpi_prcnam(LSPWN_ABSENT, &none);
        CHECK(!(nst & 1),
              "$GETJPI-by-name does NOT resolve a name that was never spawned");
        uint32_t bst = vms_kif_getjpi_prcnam(LSPWN_SUBJECT, &none);
        CHECK(!(bst & 1),
              "the subject name is NOT in the table before lib$spawn runs");
    }

    /* CREATE: lib$spawn/NOWAIT a DCL subprocess named LSPWN_SUBJECT that WAITs
     * (stays alive for the reads). B0: prcnam is APPLIED and the returned pid is
     * the executive-assigned VMS pid. */
    struct dsc$descriptor_s cmd_d  = dsc("WAIT 00:01:00");
    struct dsc$descriptor_s prc_d  = dsc(LSPWN_SUBJECT);
    uint32_t flags = CLI$M_NOWAIT;
    uint32_t vms_pid = 0;

    fflush(stdout);
    uint32_t r = lib$spawn(&cmd_d, NULL, NULL, &flags, &prc_d, &vms_pid, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);

    CHECK(r == SS$_NORMAL, "lib$spawn/NOWAIT returns SS$_NORMAL for a created subprocess");
    CHECK(vms_pid != 0, "lib$spawn returns an executive-assigned VMS pid (not zero)");

    /* THE ANTI-INV-6 ASSERTION: a DIFFERENT reader resolves the subprocess BY
     * NAME. It is registered ONLY because lib$spawn's $CREPRC child called the
     * executive -- the DCL image it exec'd never names itself. */
    struct vms_procinfo sub;
    memset(&sub, 0, sizeof(sub));
    uint32_t sst = vms_kif_getjpi_prcnam(LSPWN_SUBJECT, &sub);
    CHECK(sst & 1,
          "the lib$spawn'd subprocess is EXECUTIVE-REGISTERED (resolvable BY prcnam)");
    CHECK((sst & 1) && strcmp(sub.prcnam, LSPWN_SUBJECT) == 0,
          "the subprocess carries the requested prcnam");
    CHECK((sst & 1) && sub.vms_pid == vms_pid,
          "$GETJPI-by-name and lib$spawn agree on the executive VMS pid");

    /* Cross-check: resolve the SAME process BY its VMS pid. */
    struct vms_procinfo byid;
    memset(&byid, 0, sizeof(byid));
    uint32_t pst = vms_kif_getjpi_pid(vms_pid, &byid);
    CHECK((pst & 1) && strcmp(byid.prcnam, LSPWN_SUBJECT) == 0,
          "$GETJPI-by-pid resolves the same registered subprocess");

    /* Cleanup: terminate the subprocess (it is a genuine Linux child of ours)
     * and reap it, so it does not linger for the next suite. */
    if ((sst & 1) && sub.linux_pid) {
        kill((pid_t)sub.linux_pid, SIGKILL);
        int wst; while (waitpid((pid_t)sub.linux_pid, &wst, 0) < 0 && errno == EINTR) ;
    }

    /*
     * POSITIVE PROOF: lib$spawn actually RUNS the DCL command, and its output is
     * OBSERVABLE. This is the "SPAWN runs a command" proof MOVED here from the
     * plain-host dcl suite (conductor ruling, vms-e9a): SPAWN/lib$spawn require
     * the executive ($CREPRC), so that proof can only be made on THIS harness --
     * the plain-host suite now asserts the honest %DCL-F-CREPRC failure instead.
     * A real DCL child runs WRITE SYS$OUTPUT with SYS$OUTPUT redirected to a
     * file; lib$spawn WAITs for completion; the file must carry the marker --
     * sourced from the child's redirected output, never fabricated here.
     */
    {
        const char *OUT = "/tmp/ovmx_lspwn_out.txt";
        unlink(OUT);
        struct dsc$descriptor_s runcmd = dsc("WRITE SYS$OUTPUT \"LSPWN_RAN_OK\"");
        struct dsc$descriptor_s outf   = dsc(OUT);
        uint32_t rpid = 0, rcomp = 0;
        uint32_t rr = lib$spawn(&runcmd, NULL, &outf, NULL, NULL,
                                &rpid, &rcomp, NULL, NULL, NULL, NULL, NULL, NULL);
        CHECK(rr == SS$_NORMAL,
              "lib$spawn (wait) runs a DCL command to completion under the executive");

        char body[4096];
        long n = -1;
        int ofd = open(OUT, O_RDONLY);
        if (ofd >= 0) {
            n = 0;
            for (;;) {
                ssize_t g = read(ofd, body + n, sizeof(body) - 1 - (size_t)n);
                if (g <= 0) break;
                n += g;
            }
            body[n > 0 ? n : 0] = '\0';
            close(ofd);
        }
        CHECK(n > 0 && strstr(body, "LSPWN_RAN_OK") != NULL,
              "the DCL command's SYS$OUTPUT carries its output (the command really ran)");
        unlink(OUT);
    }

    printf("=== test_syssvc_libspawn_reg: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
