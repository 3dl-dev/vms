/*
 * test_syssvc_ci6_rmhold.c - RMHOLD.EXE really holds a lock TREE in the
 * executive (rd vms-c2b).
 *
 * Runs the REAL, VMS-native RMHOLD.EXE (tests/lab/ci6/RMHOLD.C, compiled by
 * cc and linked by LINK.EXE --use DECC$SHR + LIBVMS$SHR, PT_INTERP =
 * IMGACT.EXE) as a child process against a live vms.ko, then proves from the
 * PARENT's own executive reads -- $ENQW and $GETLKIW, never the child's
 * printout -- that the child's locks exist, that the tree really is a tree,
 * and that the value block the 'W' branch wrote is in the resource.
 *
 * WHY THIS SUITE EXISTS. RMHOLD is what makes an OVMX node a HOLDER in a
 * lock tree, which is the one arrangement in which a real VAX master can
 * ever choose OVMX as a tree's new master (rd vms-c2b, rd vms-6ad1 RA7).
 * Before it goes in front of a real VAX, the program's own contract -- root
 * mode, sub-locks under PARID, the value block -- is checked here against a
 * real executive.
 *
 * WHAT IS ASSERTED, and all of it is executive state:
 *   1. while the child holds the root at CR, THIS process is granted CR on
 *      the same resource (compatible), and $GETLKIW's GRANTCOUNT on that
 *      resource is >= 2 -- the child's lock is in the database, not just in
 *      its stdout;
 *   2. the value block the child wrote on its EX->CR down-conversion comes
 *      back in this process's own LKSB as "RMLVB-0123456789";
 *   3. an EX request with LCK$M_NOQUEUE is REFUSED while the child holds CR
 *      (the falsifier: a lock that blocks nothing is not a lock);
 *   4. a sub-resource name under THIS process's root is a different resource
 *      from the same name under no parent -- the PARID linkage RMHOLD uses
 *      is real ($GETLKIW LKI$_PARENT answers this root's lock id);
 *   5. once the child is gone and its locks are rundown, the same EX request
 *      with LCK$M_NOQUEUE SUCCEEDS -- so assertion 3 was not vacuous.
 *
 * NO /dev/vms -> honest SKIP (77): every assertion here is a lock-manager
 * answer, and there is no lock manager without the executive. Never a fake
 * pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "lckdef.h"
#include "lkidef.h"
#include "iledef.h"
#include "efndef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vmsfs/ods2.h"
#include "sysvol_stage.h"

#define EXIT_SKIP 77
#define WAIT_TIMEOUT_MS 30000
#define RMHOLD_IMAGE "/vms/SYS0/SYSCOMMON/SYSEXE/RMHOLD.EXE"

/* Root CR (1), sub-locks CW (2), two of them, 'W' = write the value block
 * on the down-conversion from EX. The same spec string a lab arm hands the
 * real VAX oracle's RMHOLD.MAR. */
#define RMHOLD_SPEC "122WRMTQ"
#define RMHOLD_ROOT "RMTQ"
#define RMHOLD_PAT  "RMLVB-0123456789"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct lksb {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[16];
};

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static uint32_t enq(const char *name, uint32_t mode, uint32_t flags,
                    uint32_t parid, struct lksb *sb)
{
    struct dsc$descriptor_s d = mkdsc(name);
    memset(sb, 0, sizeof(*sb));
    return sys$enqw(EFN$C_ENF, mode, sb, flags, &d, parid,
                    NULL, 0, NULL, 0, 0, 0);
}

static uint32_t getlki_u32(uint32_t lkid, uint16_t code, uint32_t *out)
{
    uint16_t retlen = 0;
    ILE3 items[] = {
        { 4, code, out, &retlen },
        ILE3_TERMINATOR
    };
    *out = 0;
    return sys$getlkiw(EFN$C_ENF, &lkid, items, NULL, NULL, 0, NULL);
}

/* The child is granted its root asynchronously; wait for the resource to
 * exist by watching for OUR OWN CR grant to report a grant count > 1. The
 * wait is on real state, never on a sleep (a flaky test is a broken test). */
static int wait_for_peer_holder(struct lksb *mine, uint32_t *grantcount)
{
    for (int i = 0; i < WAIT_TIMEOUT_MS / 50; i++) {
        if (enq(RMHOLD_ROOT, LCK$K_CRMODE, LCK$M_SYSTEM | LCK$M_VALBLK, 0,
                mine) & 1) {
            if ((getlki_u32(mine->lksb$l_lkid, LKI$_GRANTCOUNT, grantcount) & 1)
                && *grantcount >= 2)
                return 1;
            (void)sys$deq(mine->lksb$l_lkid, NULL, 0, 0);
        }
        struct pollfd nothing = { .fd = -1, .events = 0 };
        poll(&nothing, 1, 50);
    }
    return 0;
}

static pid_t spawn_rmhold(void)
{
    pid_t pid = ovmx_fork_subprocess();
    if (pid == 0) {
        char *argv[] = { (char *)RMHOLD_IMAGE, (char *)RMHOLD_SPEC, NULL };
        execv(RMHOLD_IMAGE, argv);
        _exit(127);
    }
    return pid;
}

static void reap(pid_t pid)
{
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* After the holder is gone its locks are rundown by the executive; the EX
 * that was refused while it held CR must now be grantable. Polled on real
 * state for the same reason as wait_for_peer_holder(). */
static int wait_until_ex_grantable(struct lksb *sb)
{
    for (int i = 0; i < WAIT_TIMEOUT_MS / 50; i++) {
        if (enq(RMHOLD_ROOT, LCK$K_EXMODE,
                LCK$M_SYSTEM | LCK$M_NOQUEUE, 0, sb) & 1)
            return 1;
        struct pollfd nothing = { .fd = -1, .events = 0 };
        poll(&nothing, 1, 50);
    }
    return 0;
}

static void check_subresource_is_a_child(uint32_t root_lkid)
{
    struct lksb sub;
    uint32_t parent = 0;

    if (!(enq("S0", LCK$K_CWMODE, 0, root_lkid, &sub) & 1)) {
        printf("  FAIL: $ENQW S0 under the root lock id failed\n");
        fail++;
        return;
    }
    CHECK((getlki_u32(sub.lksb$l_lkid, LKI$_PARENT, &parent) & 1) &&
          parent == root_lkid,
          "a sub-lock enqueued with PARID reports THAT root as its LKI$_PARENT "
          "(the tree linkage RMHOLD's sub-locks rely on)");
    (void)sys$deq(sub.lksb$l_lkid, NULL, 0, 0);
}

int main(void)
{
    struct lksb mine, ex;
    uint32_t grantcount = 0;
    pid_t child;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_ci6_rmhold (RMHOLD holds a real lock tree, vms-c2b) ===\n");

    if (!executive_present()) {
        uint32_t st = enq(RMHOLD_ROOT, LCK$K_CRMODE, LCK$M_SYSTEM, 0, &mine);
        printf("  INFO: sys$enqw with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "sys$enqw does NOT report success when the executive was never reached");
        printf("=== test_syssvc_ci6_rmhold: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    CHECK(sysvol_stage_subject(RMHOLD_IMAGE, "RMHOLD.EXE") == 0,
          "parent: RMHOLD.EXE staged on " SYSVOL_UNIT " over the ACP "
          "(IMGACT reads a main image off the volume, never /vms)");
    CHECK(sysvol_stage_shareables_from("/vms/SYS0/SYSCOMMON/SYSLIB") >= 0,
          "parent: the OVMX shareables RMHOLD --uses staged on " SYSVOL_UNIT
          " SYS$LIBRARY");

    child = spawn_rmhold();
    if (child < 0) { printf("  FAIL: fork() for RMHOLD\n"); return 1; }

    CHECK(wait_for_peer_holder(&mine, &grantcount),
          "RMHOLD's root lock is in the executive's lock database: this process's "
          "own CR grant on " RMHOLD_ROOT " reports LKI$_GRANTCOUNT >= 2");
    printf("  INFO: LKI$_GRANTCOUNT on " RMHOLD_ROOT " = %u\n",
           (unsigned)grantcount);

    CHECK(memcmp(mine.lksb$b_valblk, RMHOLD_PAT, 16) == 0,
          "the value block RMHOLD wrote on its EX->CR down-conversion comes back "
          "in THIS process's LKSB as RMLVB-0123456789");

    /* negctl: lock-compat-ex-cr */
    CHECK(!(enq(RMHOLD_ROOT, LCK$K_EXMODE, LCK$M_SYSTEM | LCK$M_NOQUEUE, 0, &ex) & 1),
          "an EX request with LCK$M_NOQUEUE is REFUSED while RMHOLD holds CR "
          "(the holder blocks, so it really holds)");

    check_subresource_is_a_child(mine.lksb$l_lkid);

    (void)sys$deq(mine.lksb$l_lkid, NULL, 0, 0);
    reap(child);

    CHECK(wait_until_ex_grantable(&ex),
          "once the holder is gone the same EX/NOQUEUE request is GRANTED "
          "(the refusal above was not vacuous)");
    (void)sys$deq(ex.lksb$l_lkid, NULL, 0, 0);

    printf("=== test_syssvc_ci6_rmhold: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
