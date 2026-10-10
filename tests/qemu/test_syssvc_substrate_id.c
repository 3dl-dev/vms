/*
 * test_syssvc_substrate_id.c - the executive gives each VMS process its own
 * unprivileged substrate identity (rd vms-ac48, epic vms-8e6) -- against a
 * real /dev/vms.
 *
 * A registered process asks for its substrate uid (GET): one from the
 * dedicated range, never 0, stable on a second GET, different from another
 * process's. BECOME makes it the task's real/effective/saved uid and gid with
 * no capabilities -- done by the executive, the task asks for nothing else.
 * A fork of that process, registering on its own, takes the parent process's
 * UIC from the executive (not from its uid), so the substrate identity carries
 * no VMS meaning.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

static int passed, failed;
static void check(int c, const char *m)
{
    if (c) { passed++; printf("  PASS: %s\n", m); }
    else   { failed++; printf("  FAIL: %s\n", m); }
}

static int in_range(uint32_t u)
{
    return u >= OVMX_SUBST_UID_BASE && u < OVMX_SUBST_UID_BASE + OVMX_SUBST_UID_SPAN;
}

/* Linux: the effective capability mask; NetBSD has none (0). */
static unsigned long long capeff(void)
{
    unsigned long long c = 0;
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "CapEff:", 7))
            sscanf(line + 7, " %llx", &c);
    fclose(f);
    return c;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_substrate_id: the executive's substrate identity ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_substrate_id: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    uint32_t mine = 0, again = 0;
    /* negctl: subst-uid-may-be-zero */
    check((vms_kif_substrate_id(VMS_SUBST_OP_GET, &mine) & 1) && mine != 0 && in_range(mine),
          "GET gives this process a substrate uid from the dedicated range, never 0");
    check((vms_kif_substrate_id(VMS_SUBST_OP_GET, &again) & 1) && again == mine,
          "a second GET gives the same uid");

    int p2c[2];
    if (pipe(p2c) != 0)
        return 1;
    pid_t pid = fork();
    if (pid == 0) {
        /* The child: a process of its own (registers fresh), then BECOMEs. */
        uint32_t u = 0, r[5] = { 0 };
        struct vms_procinfo pi;
        close(p2c[0]);
        if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL))
            _exit(2);
        r[0] = (vms_kif_substrate_id(VMS_SUBST_OP_GET, &u) & 1) ? u : 0;
        r[1] = vms_kif_substrate_id(VMS_SUBST_OP_BECOME, NULL);
        r[2] = (getuid() == u && geteuid() == u && getgid() == u && getegid() == u) ? 1 : 0;
        r[3] = capeff() == 0;
        memset(&pi, 0, sizeof pi);
        r[4] = (vms_kif_getjpi_self(&pi) & 1) ? pi.uic : 0;
        /* A fork of the child that registers on its own takes the child's
         * UIC from the executive (it now runs under the child's uid). */
        int g2c[2];
        uint32_t guic = 0;
        if (pipe(g2c) == 0) {
            pid_t g = fork();
            if (g == 0) {
                struct vms_procinfo gi;
                close(g2c[0]);
                vms_kif_close();
                memset(&gi, 0, sizeof gi);
                uint32_t x = (vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) &&
                              (vms_kif_getjpi_self(&gi) & 1)) ? gi.uic : 0xFFFFFFFFu;
                (void)!write(g2c[1], &x, sizeof x);
                _exit(0);
            }
            close(g2c[1]);
            (void)!read(g2c[0], &guic, sizeof guic);
            waitpid(g, NULL, 0);
        }
        (void)!write(p2c[1], r, sizeof r);
        (void)!write(p2c[1], &guic, sizeof guic);
        _exit(0);
    }
    close(p2c[1]);
    uint32_t r[5] = { 0 }, guic = 0;
    int got = (read(p2c[0], r, sizeof r) == (ssize_t)sizeof r) &&
              (read(p2c[0], &guic, sizeof guic) == (ssize_t)sizeof guic);
    waitpid(pid, NULL, 0);
    check(got, "the child process reported back");
    check(in_range(r[0]) && r[0] != mine,
          "another VMS process gets a different substrate uid of its own");
    check(r[1] == SS$_NORMAL, "BECOME is SS$_NORMAL (the executive changes the task's identity)");
    /* negctl: subst-become-ids-not-set */
    check(r[2] == 1, "after BECOME the task's real and effective uid and gid are that uid");
    check(r[3] == 1, "after BECOME the task holds no effective capability");
    check(guic == r[4] && guic != 0xFFFFFFFFu,
          "a fork running under that uid that registers on its own takes the same UIC from the executive");

    printf("=== test_syssvc_substrate_id: %d passed, %d failed ===\n", passed, failed);
    return failed ? 1 : 0;
}
