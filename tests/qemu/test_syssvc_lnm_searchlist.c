/*
 * test_syssvc_lnm_searchlist.c - multi-value (search-list) executive-resident
 * logical names, cross-process (vms-420).
 *
 * ============================================================
 * THE GAP THIS PROVES FIXED. vms-420: DEFINE FOO BAR,BAZ followed by SHOW
 * LOGICAL FOO created "FOO" = "BAR" with BAZ silently dropped. The executive
 * (src/kernel/vms_lnm.h/.c) already stored up to VMS_LNM_MAX_EQUIV
 * equivalence strings per entry and the DEFINE ioctl already wrote all of
 * them -- the gap was entirely userspace, in three places:
 *
 *   1. vms_kif_lnm_translate() / vms_kif_lnm_enumerate() (src/libvmssys)
 *      read only equivalence index 0 out of the mmap'd arena.
 *   2. lnm_create_multi() (src/vmslnm/lnm_client.c) never routed
 *      SYSTEM/GROUP/JOB scope through the executive at all -- unlike
 *      lnm_create()'s single-value path, it fell straight through to the
 *      PROCESS-PRIVATE local table, so a multi-value DEFINE/SYSTEM was
 *      invisible to every other process (exactly SYS$STARTUP's shape).
 *   3. DCL's cmd_define/cmd_show_logical (src/vmsdcl) never read past
 *      params[1] and never displayed more than one value.
 *
 * This suite proves (1) and (2) cross-process, at the vmslnm manager layer
 * (lnm_create_multi / lnm_translate_values / lnm_enumerate) that both DCL's
 * cmd_define/cmd_show_logical AND vms-21a's SYS$STARTUP consumer sit on top
 * of -- the same "two separate processes, one real /dev/vms" proof shape
 * test_syssvc_lnm_crossproc.c uses for the single-value case (CLAUDE.md
 * Rule 9). (3), the DCL-level rendering, is covered by
 * tests/dcl/test_show_logical_multi.sh against LNM$PROCESS on the host --
 * SHOW LOGICAL's own qualifiers already loop every table through the SAME
 * lnm_translate_values() this suite exercises for SYSTEM.
 *
 * NAMED EXACTLY LIKE SYS$STARTUP'S REAL SHAPE (docs/design-boot-faithful.md
 * §3.2): two equivalence strings, a SYS$SYSROOT-style directory then
 * SYS$MANAGER, defined via DEFINE/SYSTEM's underlying vmslnm entry point.
 * The name itself is namespaced (OVMX5B7$STARTUP) so this suite cannot
 * collide with any real SYS$STARTUP another suite or STARTUP.COM itself
 * may have seeded in the same booted guest.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "vms/logical.h"

#define EXIT_SKIP 77
#define PEER_TIMEOUT_MS 20000

#define TEST_NAME "OVMX5B7$STARTUP"
#define TEST_VAL0 "SYS$SYSROOT:[OVMX5B7_STARTUP]"
#define TEST_VAL1 "SYS$MANAGER"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct child_msg { uint32_t pass; uint32_t fail; };

static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) return 0;
        if (pr < 0) return -1;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 1;
}

static int send_token(int fd, char tok) { return write(fd, &tok, 1) == 1 ? 0 : -1; }

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* SHOW LOGICAL's enumerate path (vms-96e2/vms-420): confirm the listed
 * entry carries BOTH equivalence strings, in order, not just index 0. */
struct enum_probe { const char *want_name; int found; int values_ok; };

static int enum_probe_cb(const char *name, const lnm_entry_t *entry, void *ctx)
{
    struct enum_probe *p = (struct enum_probe *)ctx;
    if (strcmp(name, p->want_name) != 0)
        return 0;
    p->found = 1;
    p->values_ok = (entry->num_translations == 2) &&
                   (strcmp(entry->translations[0].value, TEST_VAL0) == 0) &&
                   (strcmp(entry->translations[1].value, TEST_VAL1) == 0);
    return 0;
}

/* The child: a SEPARATE process, its own executive PCB. Everything it reads
 * must come from vms.ko's shared arena, never a process-private copy. */
static int run_child(int c2p_write, int p2c_read)
{
    char tok;
    uint32_t st;

    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'P') {
        printf("  FAIL: child: never saw the parent's define token\n");
        fail++;
    } else {
        /* lnm_translate_values: the multi-value read DCL's SHOW LOGICAL
         * (named-lookup path) and vms-21a's SYS$STARTUP consumer both use. */
        char values[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t n = 0;
        st = lnm_translate_values(lnm_get_manager(), LNM_SYSTEM_TABLE,
                                  TEST_NAME, values, LNM_MAX_SEARCHLIST,
                                  &n, NULL);
        printf("  INFO: child: lnm_translate_values(LNM$SYSTEM, %s) status=%u n=%u\n",
               TEST_NAME, st, n);
        CHECK(st == SS$_NORMAL && n == 2,
              "child: the parent's SYSTEM search-list logical reports BOTH equivalence strings here");
        CHECK(n == 2 && strcmp(values[0], TEST_VAL0) == 0,
              "child: equivalence index 0 matches the parent's define");
        CHECK(n == 2 && strcmp(values[1], TEST_VAL1) == 0,
              "child: equivalence index 1 (previously dropped, vms-420) matches the parent's define");

        /* Raw kif-layer read, index by index, exactly as an iterative
         * consumer (F$TRNLNM-by-index, or vms-21a's search-list walk) would
         * use it: index 0 and 1 hit, index 2 reports "no more" but still
         * hands back num_equiv=2 so the walk knows where the list ends. */
        char kbuf[LNM_MAX_VALUE + 1];
        uint16_t klen = 0;
        uint8_t kn = 0;
        int r0 = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, TEST_NAME, 0,
                                       kbuf, sizeof(kbuf), &klen, NULL, &kn);
        CHECK(r0 == 1 && kn == 2 && strcmp(kbuf, TEST_VAL0) == 0,
              "child: vms_kif_lnm_translate index 0 matches and reports num_equiv=2");
        int r1 = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, TEST_NAME, 1,
                                       kbuf, sizeof(kbuf), &klen, NULL, &kn);
        CHECK(r1 == 1 && kn == 2 && strcmp(kbuf, TEST_VAL1) == 0,
              "child: vms_kif_lnm_translate index 1 matches and reports num_equiv=2");
        int r2 = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, TEST_NAME, 2,
                                       kbuf, sizeof(kbuf), &klen, NULL, &kn);
        CHECK(r2 == 0 && kn == 2,
              "child: vms_kif_lnm_translate index 2 (past the end) reports \"no more\", not a fabricated third value");

        /* SHOW LOGICAL's enumerate path: the listing must show both values. */
        struct enum_probe ep = { TEST_NAME, 0, 0 };
        uint32_t est = lnm_enumerate(lnm_get_manager(), LNM_SYSTEM_TABLE,
                                     enum_probe_cb, &ep);
        printf("  INFO: child: lnm_enumerate(LNM$SYSTEM) status=%u found=%d values_ok=%d\n",
               est, ep.found, ep.values_ok);
        CHECK(est == SS$_NORMAL && ep.found && ep.values_ok,
              "child: SHOW LOGICAL's enumerate path lists BOTH equivalence strings for the parent's SYSTEM search list");
    }

    struct child_msg msg;
    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return 1;
    return fail > 0 ? 1 : 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int p2c[2], c2p[2];
    uint32_t st;

    printf("=== test_syssvc_lnm_searchlist (multi-value LNM$SYSTEM search-list logicals, cross-process, vms-420) ===\n");

    if (!executive_present()) {
        const char *vals[2] = { TEST_VAL0, TEST_VAL1 };
        printf("  FAIL: parent: cannot open /dev/vms\n");
        st = lnm_create_multi(lnm_get_manager(), LNM_SYSTEM_TABLE, TEST_NAME,
                              vals, 2, LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
        printf("  INFO: lnm_create_multi(LNM$SYSTEM) with no executive returned status %u\n", st);
        CHECK(st == SS$_NOSUCHDEV,
              "parent: lnm_create_multi in LNM$SYSTEM fails honestly with SS$_NOSUCHDEV when the executive is unreachable (no per-process fallback, INV-6)");
        printf("=== test_syssvc_lnm_searchlist: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* Negative control: a never-defined name is absent, not executive-absent. */
    {
        char values[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t n = 0;
        st = lnm_translate_values(lnm_get_manager(), LNM_SYSTEM_TABLE,
                                  "OVMX5B7$NEVER_DEFINED", values,
                                  LNM_MAX_SEARCHLIST, &n, NULL);
        CHECK(st == SS$_NOLOGNAM && n == 0,
              "parent: a never-defined LNM$SYSTEM search-list name is absent (SS$_NOLOGNAM), not an executive-absent error");
    }

    if (pipe(p2c) < 0 || pipe(c2p) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        _exit(run_child(c2p[1], p2c[0]));
    }
    close(p2c[0]); close(c2p[1]);

    /* Parent: DEFINE/SYSTEM a two-value search-list logical, exactly the
     * shape of DEFINE/SYSTEM SYS$STARTUP SYS$SYSROOT:[SYS$STARTUP],
     * SYS$MANAGER (docs/design-boot-faithful.md §3.2 / vms-21a). */
    const char *vals[2] = { TEST_VAL0, TEST_VAL1 };
    st = lnm_create_multi(lnm_get_manager(), LNM_SYSTEM_TABLE, TEST_NAME,
                          vals, 2, LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
    printf("  INFO: parent: lnm_create_multi(LNM$SYSTEM, %s, {%s, %s}) returned status %u\n",
           TEST_NAME, TEST_VAL0, TEST_VAL1, st);
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "parent: lnm_create_multi in LNM$SYSTEM (the DEFINE/SYSTEM search-list path) reported success");

    /* Parent reads its own define back with all values, before handing off
     * to the child -- this is the SAME entry point DCL's SHOW LOGICAL uses. */
    {
        char values[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t n = 0;
        uint32_t rst = lnm_translate_values(lnm_get_manager(), LNM_SYSTEM_TABLE,
                                            TEST_NAME, values, LNM_MAX_SEARCHLIST,
                                            &n, NULL);
        CHECK(rst == SS$_NORMAL && n == 2 &&
              strcmp(values[0], TEST_VAL0) == 0 && strcmp(values[1], TEST_VAL1) == 0,
              "parent: reads its own multi-value SYSTEM define back with both values, in order");
    }

    if (send_token(p2c[1], 'P') < 0) fail++;

    struct child_msg cm = {0, 0};
    int r = read_bounded(c2p[0], &cm, sizeof(cm), PEER_TIMEOUT_MS);
    if (r != 1) {
        printf("  FAIL: parent: no final report from the child (r=%d)\n", r);
        fail++;
    } else {
        pass += (int)cm.pass;
        fail += (int)cm.fail;
    }

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    /* Tidy up so a re-run in the same booted guest starts clean. */
    (void)vms_kif_lnm_delete(VMS_LNM_TBL_SYSTEM, TEST_NAME, 0 /* PSL_C_EXEC */);

    printf("=== test_syssvc_lnm_searchlist: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
