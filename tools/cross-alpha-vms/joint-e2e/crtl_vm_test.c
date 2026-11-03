/*
 * crtl_vm_test.c - the DEC C RTL heap is VMS address space (rd vms-bc53).
 *
 * An ordinary 32-bit DEC C program against DECC$SHR. Its malloc/free (musl
 * mallocng) get their pages from the executive's $EXPREG/$DELTVA
 * (src/vmslink/ovmx_decc_vm.c), charged to the process's PGFLQUOTA. Run in a
 * logged-in SYSTEM session -- whose PGFLQUOTA is SOURCED from SYSUAF by
 * LOGINOUT -- as a foreign command with the argument "quota", it:
 *   1. mallocs 1 MB at a time until malloc returns NULL, which must happen
 *      well before the 400 MB bound and with errno ENOMEM (a heap on Linux
 *      mmap runs to the bound: the P0 window and the guest's memory hold
 *      400 untouched megabytes);
 *   2. asks SYS$EXPREG for what that malloc asked ($EXPREG of 1 MB + 8 KB, 2064 pagelets): the EXECUTIVE must refuse it
 *      SS$_EXQUOTA -- the heap's pages and SYS$EXPREG's are one charge, so a
 *      heap that was not charged leaves room and SYS$EXPREG succeeds;
 *   3. frees everything and asks again: the charge came back, so SYS$EXPREG
 *      and a fresh malloc succeed.
 * Run with no argument (SYSTARTUP, a process with no sourced quota) it only
 * checks that small and large allocations work and lie in P0.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned int sys$expreg(unsigned int pagcnt, void *retadr, unsigned int acmode,
                        unsigned int region);

#define SS_EXQUOTA 28u
#define NMAX 400

static char *blk[NMAX];

int main(int argc, char **argv)
{
    unsigned long long ret[4];
    unsigned int st1, st2;
    char *a, *b, *p;
    int n, i, e, ok;

    a = malloc(100);
    b = malloc(300000);
    ok = a && b && (unsigned long)a < 0x40000000UL && (unsigned long)b < 0x40000000UL;
    if (b) memset(b, 0x5A, 300000);
    printf("CRTLVM: small+large malloc in P0=%d\n", ok);
    free(b);

    if (argc < 2 || strcmp(argv[1], "quota") != 0) {
        printf("CRTLVM: boot run (no quota probe)\n");
        return ok ? 1 : 44;
    }

    for (n = 0; n < NMAX; n++) {
        errno = 0;
        blk[n] = malloc(1024 * 1024);
        if (!blk[n])
            break;
        memset(blk[n], n & 0xFF, 512);
    }
    e = errno;
    memset(ret, 0, sizeof ret);
    st1 = sys$expreg(2064, ret, 0, 0);   /* 1 MB + one Alpha page: what the refused malloc asked for */
    printf("CRTLVM: heap exhausted after %d MB: malloc NULL errno=%s, then SYS$EXPREG %%X%08X\n",
           n, e == ENOMEM ? "ENOMEM" : "other", st1);

    for (i = 0; i < n; i++)
        free(blk[i]);
    memset(ret, 0, sizeof ret);
    st2 = sys$expreg(2064, ret, 0, 0);
    p = malloc(1024 * 1024);
    printf("CRTLVM: after free: SYS$EXPREG %%X%08X, malloc 1 MB %s\n",
           st2, p ? "ok" : "NULL");

    ok = ok && n >= 1 && n < NMAX && e == ENOMEM && st1 == SS_EXQUOTA &&
         (st2 & 1) && p != NULL;
    printf("OVMX CRTL-VM test: %s\n", ok ? "OK (the heap is P0 charged to PGFLQUOTA)" : "FAILED");
    printf("CRTLVM-END\n");
    return ok ? 1 : 44;
}
