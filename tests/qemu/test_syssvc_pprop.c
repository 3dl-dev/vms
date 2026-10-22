/*
 * test_syssvc_pprop.c - $SET_PROCESS_PROPERTIESW's parse style is the EXECUTIVE's
 * per-process attribute (rd vms-b9c, epic vms-810).
 *
 * The file-name parse style (PPROP$C_PARSE_STYLE_PERM / _TEMP, values
 * PARSE_STYLE$C_TRADITIONAL / _EXTENDED, docs/oracle/alpha84-starlet-defs/
 * PPROPDEF.txt) decides whether RMS keeps the case of a file name (EXTENDED) or
 * upper-cases it (TRADITIONAL) -- observed on the real Alpha V8.4 system,
 * tests/lab/captures/ods5-alpha-20261022/RESULTS.md session 3 (R1/R2).
 *
 * Proves, through the public sys$set_process_propertiesw and the raw
 * VMS_IOCTL_PPROP:
 *   1. a process that never set one is TRADITIONAL;
 *   2. setting PARSE_STYLE_PERM to EXTENDED returns the previous value, and both
 *      the permanent and the current image's style then read EXTENDED;
 *   3. PARSE_STYLE_TEMP changes the current image's style only;
 *   4. an image the process activates (REGISTER_CONTINUE child) starts at the
 *      PERMANENT style -- the TEMP setting does not outlive its image;
 *   5. a freshly created process (not a continuation) is TRADITIONAL, so the value
 *      is the executive's per-process attribute, not a global;
 *   6. an unknown property, or a value that is not a parse style, is
 *      SS$_BADPARAM and changes nothing.
 * Without /dev/vms: honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>
#include "starlet.h"
#include "ssdef.h"
#include "ppropdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static uint32_t rd(uint32_t prop)
{
    uint32_t v = 0xFFFFFFFFu;
    if (!(vms_kif_pprop(prop, NULL, &v) & 1))
        return 0xFFFFFFFFu;
    return v;
}

/* child: a continuation or a fresh process; report its {perm, image} styles */
static int child_read(int cont, uint32_t out[2])
{
    int p[2];
    if (pipe(p) != 0) return -1;
    struct vms_procinfo me;
    memset(&me, 0, sizeof(me));
    (void)vms_kif_getjpi_self(&me);
    pid_t k = cont ? ovmx_fork_continue()
                   : ovmx_fork_detached_st(me.username, me.uic, me.perm_privs, NULL);
    if (k == 0) {
        uint32_t v[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        close(p[0]);
        vms_kif_close();                   /* the child gets its own /dev/vms descriptor */
        if (vms_kif_open() >= 0 && (ovmx_test_self(NULL) & 1)) {
            v[0] = rd(PPROP$C_PARSE_STYLE_PERM);
            v[1] = rd(PPROP$C_PARSE_STYLE_TEMP);
        }
        (void)!write(p[1], v, sizeof v);
        _exit(0);
    }
    close(p[1]);
    ssize_t n = read(p[0], out, 2 * sizeof out[0]);
    close(p[0]);
    int st; waitpid(k, &st, 0);
    return n == (ssize_t)(2 * sizeof out[0]) ? 0 : -1;
}

int main(void)
{
    uint64_t prev = 99;
    uint32_t st, v[2], bad = 7;

    printf("=== test_syssvc_pprop ===\n");
    if (vms_kif_open() < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    if (!(ovmx_test_self(NULL) & 1)) { printf("  FAIL: register\n"); return 1; }

    CHECK(rd(PPROP$C_PARSE_STYLE_PERM) == PARSE_STYLE$C_TRADITIONAL &&
          rd(PPROP$C_PARSE_STYLE_TEMP) == PARSE_STYLE$C_TRADITIONAL,
          "a process that never set one is TRADITIONAL (permanent and image)");

    st = sys$set_process_propertiesw(NULL, NULL, NULL, PPROP$C_PARSE_STYLE_PERM,
                                     PARSE_STYLE$C_EXTENDED, &prev, 0, 0);
    CHECK((st & 1) && prev == PARSE_STYLE$C_TRADITIONAL,
          "$SET_PROCESS_PROPERTIESW PARSE_STYLE_PERM=EXTENDED returns the previous TRADITIONAL");
    /* negctl: pprop-not-stored */
    CHECK(rd(PPROP$C_PARSE_STYLE_PERM) == PARSE_STYLE$C_EXTENDED &&
          rd(PPROP$C_PARSE_STYLE_TEMP) == PARSE_STYLE$C_EXTENDED,
          "a later read returns EXTENDED for the process and the current image");

    st = sys$set_process_propertiesw(NULL, NULL, NULL, PPROP$C_PARSE_STYLE_TEMP,
                                     PARSE_STYLE$C_TRADITIONAL, &prev, 0, 0);
    CHECK((st & 1) && prev == PARSE_STYLE$C_EXTENDED &&
          rd(PPROP$C_PARSE_STYLE_TEMP) == PARSE_STYLE$C_TRADITIONAL &&
          rd(PPROP$C_PARSE_STYLE_PERM) == PARSE_STYLE$C_EXTENDED,
          "PARSE_STYLE_TEMP=TRADITIONAL changes the image's style only");

    CHECK(child_read(1, v) == 0 && v[0] == PARSE_STYLE$C_EXTENDED && v[1] == PARSE_STYLE$C_EXTENDED,
          "an activated image (REGISTER_CONTINUE) starts at the PERMANENT style, EXTENDED");
    CHECK(child_read(0, v) == 0 && v[0] == PARSE_STYLE$C_TRADITIONAL,
          "a freshly created process is TRADITIONAL (the executive's per-process value)");

    st = sys$set_process_propertiesw(NULL, NULL, NULL, 99, PARSE_STYLE$C_EXTENDED, &prev, 0, 0);
    CHECK(st == SS$_BADPARAM, "an unknown property is SS$_BADPARAM");
    st = vms_kif_pprop(PPROP$C_PARSE_STYLE_PERM, &bad, NULL);
    CHECK(st == SS$_BADPARAM && rd(PPROP$C_PARSE_STYLE_PERM) == PARSE_STYLE$C_EXTENDED,
          "a value that is not a parse style is SS$_BADPARAM and stores nothing");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
