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
#include "pqldef.h"
#include "descrip.h"
#include "tcg_deadline.h"
#include <fcntl.h>

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

    st = ovmx$vm_expreg(131072, &p2);
    /* negctl: va-pgflquota-unenforced */
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

/*
 * ---- 5. $CREPRC'S QUOTA LIST sources a subprocess's PGFLQUOTA (rd vms-89dd8) ----
 *
 * The creator $CREPRCs THIS binary as the subprocess; VA_CREPRC_OUT names the
 * file the subprocess reports its checks into, VA_CREPRC_WANT the PGFLQUOTA
 * (pagelets) the executive must report for it. The child holds no identity of
 * its own and sources nothing: whatever limit it sees, the executive gave it.
 */
static int creprc_child(const char *out, unsigned int want)
{
    struct vms_va_args a;
    void *r[2];
    char tmp[300], buf[256];
    int fd, bad = 0;

    snprintf(buf, sizeof buf, "creprc child: ");
    if (want == 0) {                    /* nothing loaded: left unsourced */
        if (q(&a) && a.quota_sourced == 0 && sys$expreg(64, r, 0, 0) == SS$_NORMAL)
            snprintf(buf, sizeof buf, "OK\n");
        else {
            bad++;
            snprintf(buf, sizeof buf, "BAD: sourced=%u, want unsourced\n", a.quota_sourced);
        }
    } else if (!(q(&a) && a.quota_sourced == 1 && a.pgflquota == want)) {
        bad++;
        snprintf(buf, sizeof buf, "BAD: sourced=%u pgflquota=%llu want=%u\n",
                 q(&a) ? a.quota_sourced : 99u, (unsigned long long)a.pgflquota, want);
    } else if (sys$expreg(64, r, 0, 0) != SS$_NORMAL) {
        bad++;
        snprintf(buf, sizeof buf, "BAD: a request inside the quota was refused\n");
    } else if (sys$expreg(want + 512, r, 0, 0) != SS$_EXQUOTA) {
        bad++;
        snprintf(buf, sizeof buf, "BAD: a request past the quota was not SS$_EXQUOTA\n");
    } else
        snprintf(buf, sizeof buf, "OK\n");
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        if (write(fd, buf, strlen(buf)) < 0) bad++;
        close(fd);
        rename(tmp, out);
    }
    return bad ? 1 : 0;
}

/* $CREPRC a subprocess of this binary with `quota` (NULL = none); returns the
 * subprocess's report ("OK", or its BAD line), or "" if it never reported. */
static const char *creprc_probe(const void *quota, unsigned int want, char *rep, size_t n)
{
    static int seq;
    char out[128], exe[256], want_s[16];
    struct dsc$descriptor_s img;
    uint32_t pid = 0, st;
    ssize_t k;
    int fd, t;

    snprintf(out, sizeof out, "/tmp/va_creprc.%d.%d", (int)getpid(), ++seq);
    unlink(out);
    snprintf(want_s, sizeof want_s, "%u", want);
    setenv("VA_CREPRC_OUT", out, 1);
    setenv("VA_CREPRC_WANT", want_s, 1);
    k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) { rep[0] = 0; return rep; }
    exe[k] = 0;
    img.dsc$w_length = (unsigned short)k;
    img.dsc$b_dtype = 14; img.dsc$b_class = 1; img.dsc$a_pointer = exe;
    fflush(NULL);
    st = sys$creprc(&pid, &img, NULL, NULL, NULL, NULL, quota, NULL, 0, 0, 0, 0);
    rep[0] = 0;
    if (!(st & 1)) { snprintf(rep, n, "CREPRC %08x", st); return rep; }
    for (t = 0; t < (int)ovmx_tcg_ms(30000) / 100; t++) {
        fd = open(out, O_RDONLY);
        if (fd >= 0) {
            k = read(fd, rep, n - 1);
            close(fd);
            rep[k > 0 ? k : 0] = 0;
            unlink(out);
            return rep;
        }
        usleep(100000);
    }
    return rep;
}

#define QENT(code, val) (code), \
    (unsigned char)((val) & 0xFF), (unsigned char)(((val) >> 8) & 0xFF), \
    (unsigned char)(((val) >> 16) & 0xFF), (unsigned char)(((val) >> 24) & 0xFF)

/* The SYSGEN pair this test loads into the executive (distinct from any real
 * system's, so a hard-coded default cannot pass): default 6000, minimum 3000. */
#define T_DPG 6000u
#define T_MPG 3000u

static void test_creprc_quota(void)
{
    char rep[256];
    static const unsigned char q5000[] = { QENT(PQL$_PGFLQUOTA, 5000u), PQL$_LISTEND, 0,0,0,0 };
    static const unsigned char qlow[]  = { QENT(PQL$_PGFLQUOTA, 100u), PQL$_LISTEND, 0,0,0,0 };
    static const unsigned char qother[] = { QENT(PQL$_FILLM, 50u), PQL$_LISTEND, 0,0,0,0 };
    static const unsigned char qbad[]  = { QENT(99, 1u), PQL$_LISTEND, 0,0,0,0 };
    struct dsc$descriptor_s img = { 4, 14, 1, (char *)"/x" };
    uint32_t pid = 0;

    check((vms_kif_pql_load(T_DPG, T_MPG, 0) & 1) != 0,
          "creprc: the executive accepts a SYSGEN PQL_DPGFLQUOTA/PQL_MPGFLQUOTA pair (6000/3000)");

    creprc_probe(q5000, 5000, rep, sizeof rep);
    /* negctl: creprc-quota-list-ignored */
    check(strcmp(rep, "OK\n") == 0,
          "creprc: a listed PQL$_PGFLQUOTA of 5000 sources the subprocess's PGFLQUOTA; $EXPREG past it is SS$_EXQUOTA");
    if (strcmp(rep, "OK\n")) printf("    child said: %s\n", rep);
    creprc_probe(qlow, T_MPG, rep, sizeof rep);
    check(strcmp(rep, "OK\n") == 0,
          "creprc: a listed PGFLQUOTA below the loaded PQL_MPGFLQUOTA is clamped up to it (3000)");
    if (strcmp(rep, "OK\n")) printf("    child said: %s\n", rep);
    creprc_probe(qother, T_DPG, rep, sizeof rep);
    /* negctl: creprc-pql-default-hardcoded */
    check(strcmp(rep, "OK\n") == 0,
          "creprc: a list without PGFLQUOTA takes the loaded PQL_DPGFLQUOTA (6000)");
    if (strcmp(rep, "OK\n")) printf("    child said: %s\n", rep);
    check(sys$creprc(&pid, &img, NULL, NULL, NULL, NULL, qbad, NULL, 0, 0, 0, 0) == SS$_BADPARAM,
          "creprc: an unknown PQL$_ code in the list is SS$_BADPARAM");

    check((vms_kif_pql_load(0, 0, VMS_PQL_LOAD_F_CLEAR) & 1) != 0,
          "creprc: the loaded pair can be cleared (the executive's never-loaded state)");
    creprc_probe(q5000, 0, rep, sizeof rep);
    check(strcmp(rep, "OK\n") == 0,
          "creprc: with no SYSGEN pair loaded the executive invents none: the child is left unsourced");
    if (strcmp(rep, "OK\n")) printf("    child said: %s\n", rep);
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
    if (getenv("VA_CREPRC_OUT")) {          /* the $CREPRC'd subprocess (section 5) */
        if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || vms_kif_open() < 0)
            return 1;
        return creprc_child(getenv("VA_CREPRC_OUT"), (unsigned int)atoi(getenv("VA_CREPRC_WANT")));
    }
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

    test_creprc_quota();

    printf("=== test_syssvc_va: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
