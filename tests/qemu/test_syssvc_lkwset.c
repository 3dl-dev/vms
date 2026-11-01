/*
 * test_syssvc_lkwset.c - $LKWSET / $ULWSET really lock and unlock pages
 * (vms-18c2), through the PUBLIC sys$ API.
 *
 * THE BUG THIS GUARDS. Both were validate-and-return-SS$_NORMAL no-ops that
 * echoed the caller's range back: a success that locked nothing. Now the pages
 * are mlock()ed; the test reads the process's own VmLck from /proc/self/status
 * before and after, so it does not take the service's word for it.
 * Without an executive it exits EXIT_SKIP (77), as the kernel-executive
 * negative-control job requires of every public-sys$ suite.
 * NEGATIVE CONTROL: tests/qemu/defects.d/lkwset-noop.defect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#include "starlet.h"
#include "ssdef.h"
#include "va_rangedef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static long vmlck_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmLck: %ld kB", &v) == 1) break;
    fclose(f);
    return v;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_lkwset ($LKWSET/$ULWSET lock for real) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_lkwset: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }

    long pg = sysconf(_SC_PAGESIZE);
    size_t len = 8 * (size_t)pg;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { printf("  FAIL: mmap\n"); return 1; }
    memset(p, 1, len);

    VA_RANGE in, got;
    memset(&got, 0, sizeof got);
    in.va_range$ps_start_va = p + 10;                 /* unaligned on purpose */
    in.va_range$ps_end_va   = p + len - 10;

    long before = vmlck_kb();
    uint32_t st = sys$lkwset(&in, &got, 0);
    if (st == SS$_NOPRIV) {
        /* host refused (RLIMIT_MEMLOCK): honest refusal, nothing else to prove */
        printf("  PASS: the host refused the lock and $LKWSET said SS$_NOPRIV, not success\n");
        printf("=== test_syssvc_lkwset: SS$_NOPRIV path ===\n");
        return 0;
    }
    CHECK(st == SS$_NORMAL, "$LKWSET returns SS$_NORMAL");
    long locked = vmlck_kb();
    /* negctl: lkwset-noop */
    CHECK(before >= 0 && locked - before >= (long)(len / 1024) - 2 * (pg / 1024),
          "$LKWSET really locked the pages: VmLck grew by the range");
    /* negctl: lkwset-noop */
    CHECK((uintptr_t)got.va_range$ps_start_va == ((uintptr_t)p & ~(uintptr_t)(pg - 1)) &&
          (uintptr_t)got.va_range$ps_end_va == (((uintptr_t)p + len - 10) | (uintptr_t)(pg - 1)),
          "retadr is the page-rounded range actually locked, not an echo of the input");

    st = sys$ulwset(&in, NULL, 0);
    CHECK(st == SS$_NORMAL, "$ULWSET returns SS$_NORMAL");
    CHECK(vmlck_kb() == before, "$ULWSET really unlocked them: VmLck is back to where it began");

    VA_RANGE bad = { (void *)0x10, (void *)0x20 };
    /* negctl: lkwset-noop */
    CHECK(!((st = sys$lkwset(&bad, NULL, 0)) & 1), "$LKWSET of an unmapped range is refused, not SS$_NORMAL");
    CHECK(sys$lkwset(NULL, NULL, 0) == SS$_BADPARAM, "$LKWSET with no range is SS$_BADPARAM");

    munmap(p, len);
    printf("=== test_syssvc_lkwset: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
