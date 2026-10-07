/*
 * test_syssvc_setdfprot.c - $SETDFPROT is the EXECUTIVE's per-process attribute.
 *
 * Proves, through the public sys$setdfprot and the raw VMS_IOCTL_DFPROT:
 *   1. a process that never set one reads VMS_DFPROT_INITIAL, and the status is
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
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "starlet.h"
#include "rmsdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms_ioctl.h"

#define EXIT_SKIP 77
static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static uint32_t reg(int fd, int cont)
{
    struct vms_register_args r;
    memset(&r, 0, sizeof r);
    if (ioctl(fd, cont ? VMS_IOCTL_REGISTER_CONTINUE : VMS_IOCTL_REGISTER, &r) != 0)
        return 0;
    return r.status == 1 ? r.vms_pid : 0;
}

/* child: register (continue or fresh) and report the default protection it sees */
static int child_read(int cont, uint32_t *out)
{
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t k = fork();
    if (k == 0) {
        close(p[0]);
        uint32_t v = 0xFFFFFFFFu;
        int fd = open("/dev/vms", O_RDWR);
        if (fd >= 0 && reg(fd, cont)) {
            struct vms_dfprot_args a; memset(&a, 0, sizeof a);
            if (ioctl(fd, VMS_IOCTL_DFPROT, &a) == 0 && a.status == 1) v = a.oldprot;
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
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    if (!reg(fd, 0)) { printf("  FAIL: register\n"); return 1; }
    if (vms_kif_open() < 0 || !(vms_kif_register(NULL) & 1)) { printf("  FAIL: kif register\n"); return 1; }

    uint16_t old = 0xBEEF;
    uint32_t st = sys$setdfprot(NULL, &old);
    CHECK(st == RMS$_NORMAL, "a read returns RMS$_NORMAL (0x00010001)");
    CHECK(old == VMS_DFPROT_INITIAL, "an unset process reports the initial default");

    uint16_t want = 0x0F00;
    st = sys$setdfprot(&want, &old);
    CHECK(st == RMS$_NORMAL && old == VMS_DFPROT_INITIAL, "setting returns the previous value");
    st = sys$setdfprot(NULL, &old);
    CHECK(st == RMS$_NORMAL && old == 0x0F00, "a later read returns what was set");

    uint32_t v = 0;
    CHECK(child_read(1, &v) == 0 && v == 0x0F00,
          "a REGISTER_CONTINUE child (activated image) inherits it from the executive");
    CHECK(child_read(0, &v) == 0 && v == VMS_DFPROT_INITIAL,
          "a freshly registered child does NOT (the executive, not a global, decides)");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
