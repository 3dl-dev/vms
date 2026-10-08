/*
 * test_syssvc_setdfprot.c - $SETDFPROT is the EXECUTIVE's per-process attribute.
 *
 * Proves, through the public sys$setdfprot and the raw VMS_IOCTL_DFPROT:
 *   1. a process that never set one reads the initial default 0xFF00, and the status is
 *      RMS$_NORMAL (what the Alpha V8.4 lab's service returns, not SS$_NORMAL);
 *   2. setting 0x0F00 returns the previous value and a later read returns 0x0F00;
 *   3. a child that REGISTER_CONTINUEs the parent's identity (an activated image)
 *      inherits 0x0F00 from the executive -- nothing in userspace carries it;
 *   4. a child that registers FRESH (not a continuation) reads the initial value, so
 *      the inheritance is the executive's choice and not a global.
 * Without /dev/vms: honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include "starlet.h"
#include "rmsdef.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

/* child: register (continue or fresh) and report the default protection it sees */
static int child_read(int cont, uint32_t *out)
{
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t k = fork();
    if (k == 0) {
        close(p[0]);
        uint32_t v = 0xFFFFFFFFu;
        vms_kif_close();                   /* the child gets its own /dev/vms descriptor */
        if (vms_kif_open() >= 0) {
            uint32_t rs = cont ? vms_kif_register_continue() : vms_kif_register(NULL);
            uint16_t o = 0xBEEF;
            if ((rs & 1) && (vms_kif_dfprot(NULL, &o) & 1)) v = o;
        }
        (void)!write(p[1], &v, sizeof v);
        _exit(0);
    }
    close(p[1]);
    ssize_t n = read(p[0], out, sizeof *out);
    close(p[0]);
    int st; waitpid(k, &st, 0);
    return n == (ssize_t)sizeof *out ? 0 : -1;
}

int main(void)
{
    printf("=== test_syssvc_setdfprot ===\n");
    if (vms_kif_open() < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    if (!(vms_kif_register(NULL) & 1)) { printf("  FAIL: register\n"); return 1; }

    uint16_t old = 0xBEEF;
    uint32_t st = sys$setdfprot(NULL, &old);
    CHECK(st == RMS$_NORMAL, "a read returns RMS$_NORMAL (0x00010001)");
    CHECK(old == 0xFF00u, "an unset process reports the initial default");

    uint16_t want = 0x0F00;
    st = sys$setdfprot(&want, &old);
    CHECK(st == RMS$_NORMAL && old == 0xFF00u, "setting returns the previous value");
    st = sys$setdfprot(NULL, &old);
    /* negctl: setdfprot-value-not-retained */
    CHECK(st == RMS$_NORMAL && old == 0x0F00, "a later read returns what was set");

    uint32_t v = 0;
    /* negctl: setdfprot-value-not-retained */
    CHECK(child_read(1, &v) == 0 && v == 0x0F00,
          "a REGISTER_CONTINUE child (activated image) inherits it from the executive");
    CHECK(child_read(0, &v) == 0 && v == 0xFF00u,
          "a freshly registered child does NOT (the executive, not a global, decides)");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
