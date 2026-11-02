/*
 * test_syssvc_va.c - the process virtual address space is the EXECUTIVE's:
 * $EXPREG / $CRETVA / $DELTVA and the DEC C RTL heap's address space
 * (rd vms-bc53), against a real /dev/vms.
 *
 * What is asserted, and why it can only pass if the executive did the work:
 *   1. SYS$EXPREG/CRETVA/DELTVA (LIBVMS) move the executive's per-process
 *      accounting (VMS_VA_QUERY): pagelets charged, the end of P0 (frep0va),
 *      the ranges held. A userspace mmap would leave all three unmoved.
 *   2. $CRETVA replaces pages $EXPREG made (they come back demand-zero) and
 *      refuses pages it did not make (this image's own data) SS$_PAGOWNVIO.
 *   3. In a child whose PGFLQUOTA is SOURCED (it plays LOGINOUT: SETIDENT with
 *      a quota block), the DEC C heap's VM entry points (ovmx$vm_expreg /
 *      ovmx$vm_deltva, src/vmslink/ovmx_decc_vm.c -- what DECC$SHR's malloc
 *      calls) are charged against that quota and refused SS$_EXQUOTA past it,
 *      and the same refusal comes from SYS$EXPREG; deleting returns the charge.
 *   4. The child's charges are the child's: the parent's accounting does not
 *      move (per-process state, not a counter in this image).
 * The DECC$SHR malloc -> NULL/ENOMEM end of the same path is the Alpha proof
 * (tools/cross-alpha/run-module-gp-activation-alpha.sh crtl-vm-gate).
 *
 * No /dev/vms -> SKIP (77).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "gen64def.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77

/* The DEC C heap's VM entry points (src/vmslink/ovmx_decc_vm.c). */
unsigned int ovmx$vm_expreg(unsigned long long len, unsigned long long *addr);
unsigned int ovmx$vm_deltva(unsigned long long addr, unsigned long long len);

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

static int q(struct vms_va_args *a)
{
    memset(a, 0, sizeof *a);
    a->op = VMS_VA_QUERY;
    return (vms_kif_va(a) & 1) != 0;
}

static char own_data[65536];

#define CHILD_QUOTA 256u                  /* pagelets = 128 KB */

static int child_quota(void)
{
    struct vms_procinfo info;
    struct vms_jib_quota jq;
    struct vms_va_args a;
    unsigned long long p = 0, p2 = 0;
    unsigned int st;
    int n;

    memset(&jq, 0, sizeof jq);
    jq.pgflquota = CHILD_QUOTA;
    check((vms_kif_getjpi_self(&info) & 1) &&
          (vms_kif_setident_quota(info.username[0] ? info.username : "VATEST",
                                  info.uic, info.perm_privs, &jq) & 1),
          "child: LOGINOUT's part -- SETIDENT with a PGFLQUOTA of 256 pagelets");
    check(q(&a) && a.quota_sourced == 1 && a.pgflquota == CHILD_QUOTA &&
          a.pagfilcnt == CHILD_QUOTA && a.charged == 0,
          "child: the executive reports a sourced PGFLQUOTA 256, all of it remaining");

    st = ovmx$vm_expreg(65536, &p);
    check((st & 1) && p >= VMS_VA_P0_FLOOR && p + 65536 <= VMS_VA_P0_CEILING,
          "child: the C RTL heap's $EXPREG of 64 KB lands in P0");
    if (st & 1)
        memset((void *)(uintptr_t)p, 0x5A, 65536);
    /* negctl: decc-heap-linux-mmap */
    check(q(&a) && a.charged == 128 && a.pagfilcnt == CHILD_QUOTA - 128,
          "child: the executive charged the heap's 64 KB: 128 pagelets, 128 remaining");

    /* negctl: va-pgflquota-unenforced */
    st = ovmx$vm_expreg(131072, &p2);
    check(st == SS$_EXQUOTA,
          "child: a heap request past PGFLQUOTA is refused SS$_EXQUOTA by the executive");
    check(q(&a) && a.charged == 128,
          "child: ... and nothing was charged for the refused request");
    {
        void *r[2];
        check(sys$expreg(512, r, 0, 0) == SS$_EXQUOTA,
              "child: SYS$EXPREG past PGFLQUOTA is SS$_EXQUOTA too");
    }

    check((ovmx$vm_deltva(p, 65536) & 1) && q(&a) && a.charged == 0 &&
          a.pagfilcnt == CHILD_QUOTA,
          "child: $DELTVA of the heap's pages returns their charge");

    for (n = 0; n < 64; n++) {
        st = ovmx$vm_expreg(16384, &p);
        if (!(st & 1))
            break;
    }
    check(n == 8 && st == SS$_EXQUOTA,
          "child: 16 KB at a time, the heap gets exactly 8 before SS$_EXQUOTA");

    printf("=== test_syssvc_va child: %d passed, %d failed ===\n", pass, fail);
    fflush(NULL);
    return fail ? 1 : 0;
}

int main(void)
{
    struct vms_va_args a0, a;
    void *r[2], *rr[2], *in[2];
    unsigned char *m;
    unsigned long long base, len;
    pid_t c;
    int ws = 0, i, zero;
    GENERIC_64 rid = { 0 };
    void *daddr = NULL;
    uint64_t dlen = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_va: $EXPREG/$CRETVA/$DELTVA and the C RTL heap are the executive's (rd vms-bc53) ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) { printf("  FAIL: pcb\n"); return 1; }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_va: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    check(q(&a0), "the executive answers the VA accounting query");

    /* ---- 1. $EXPREG ---- */
    check(sys$expreg(64, r, 0, 0) == SS$_NORMAL, "SYS$EXPREG 64 pagelets in P0");
    base = (uintptr_t)r[0];
    len = (uintptr_t)r[1] - base + 1;
    check(base >= VMS_VA_P0_FLOOR && (uintptr_t)r[1] < VMS_VA_P0_CEILING && len >= 64 * 512,
          "... the range is in P0 and at least 32 KB");
    check(q(&a) && a.charged == a0.charged + len / 512,
          "... the executive charged exactly the pages it created");
    check(a.frep0va == base + len, "... and the end of P0 is now the end of that range");
    m = (unsigned char *)(uintptr_t)base;
    memset(m, 0xA5, (size_t)len);

    /* ---- 2. $CRETVA ---- */
    check(sys$cretva(r, rr, 0) == SS$_NORMAL && rr[0] == r[0] && rr[1] == r[1],
          "SYS$CRETVA over pages $EXPREG made replaces them");
    for (zero = 1, i = 0; i < (int)len; i += 512)
        if (m[i]) zero = 0;
    check(zero, "... with demand-zero pages");
    check(q(&a) && a.charged == a0.charged + len / 512,
          "... and the charge did not double");
    in[0] = own_data + 8192;
    in[1] = own_data + 16383;
    check(sys$cretva(in, rr, 0) == SS$_PAGOWNVIO,
          "SYS$CRETVA over this image's own data is SS$_PAGOWNVIO");
    check(sys$expreg(16, rr, 0, 1) == SS$_UNSUPPORTED,
          "SYS$EXPREG of P1 is SS$_UNSUPPORTED, not a P0 range");

    /* ---- 3. $DELTVA ---- */
    check(sys$deltva(r, rr, 0) == SS$_NORMAL && rr[0] == r[0] && rr[1] == r[1],
          "SYS$DELTVA deletes the range and reports it");
    check(q(&a) && a.charged == a0.charged && a.extents == a0.extents,
          "... the executive returned the charge and dropped the range");
    check(sys$deltva(r, rr, 0) == SS$_NORMAL && rr[0] == (void *)(intptr_t)-1,
          "SYS$DELTVA of pages that no longer exist deletes nothing (-1)");
    check(sys$expreg(4, r, 0, 0) == SS$_NORMAL &&
          sys$deltva_64(&rid, r[0], (uint64_t)((char *)r[1] - (char *)r[0] + 1), 0,
                        &daddr, &dlen) == SS$_NORMAL &&
          daddr == r[0] && dlen == (uint64_t)((char *)r[1] - (char *)r[0] + 1),
          "SYS$DELTVA_64 deletes a $EXPREG range and echoes it");

    /* ---- 4. a sourced quota, the heap's entry points, per-process ---- */
    q(&a0);
    fflush(NULL);
    c = ovmx_fork_login();
    if (c == 0)
        _exit(child_quota());
    check(c > 0, "the executive created the quota child (a login process)");
    if (c > 0)
        while (waitpid(c, &ws, 0) < 0 && errno == EINTR)
            ;
    check(c > 0 && WIFEXITED(ws) && WEXITSTATUS(ws) == 0,
          "the quota child's checks all passed");
    check(q(&a) && a.charged == a0.charged && a.extents == a0.extents,
          "the child's heap was charged to the child: this process's accounting did not move");

    printf("=== test_syssvc_va: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
