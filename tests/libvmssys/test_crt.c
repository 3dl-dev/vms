/*
 * test_crt.c - Test full CRT bootstrap
 *
 * Verifies: _start runs, runtime init works, argc/argv/envp are correct,
 * TLS is functional, auxv is parsed, environment access works.
 */

#include "vmssys.h"

static __thread int tls_var = 42;

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    int failures = 0;

    vms_printf("=== libvmssys CRT bootstrap test ===\n");

    /* Test 1: argc/argv sanity */
    if (argc >= 1 && argv != NULL && argv[0] != NULL) {
        vms_printf("  OK: argc=%d, argv[0]=\"%s\"\n", argc, argv[0]);
    } else {
        vms_printf("  FAIL: argc/argv\n");
        failures++;
    }

    /* Test 2: Environment access */
    {
        /* PATH should almost always be set */
        char *path = vms_getenv("PATH");
        if (path) {
            vms_printf("  OK: getenv(PATH)=\"%.40s...\"\n", path);
        } else {
            /* Not fatal -- might not be set in minimal environments */
            vms_printf("  WARN: PATH not found in environment\n");
        }
    }

    /* Test 3: Page size from auxv */
    {
        if (vms_page_size == 4096 || vms_page_size == 65536) {
            vms_printf("  OK: page_size=%lu\n", vms_page_size);
        } else {
            vms_printf("  FAIL: unexpected page_size=%lu\n", vms_page_size);
            failures++;
        }
    }

    /* Test 4: TLS variable access */
    {
        if (tls_var == 42) {
            tls_var = 99;
            if (tls_var == 99) {
                vms_printf("  OK: TLS variable read/write\n");
            } else {
                vms_printf("  FAIL: TLS variable write\n");
                failures++;
            }
        } else {
            vms_printf("  FAIL: TLS variable initial value (got %d)\n", tls_var);
            failures++;
        }
    }

    /* Test 5: Auxv getauxval */
    {
        unsigned long pagesz = vms_getauxval(VMS_AT_PAGESZ);
        if (pagesz > 0) {
            vms_printf("  OK: getauxval(AT_PAGESZ)=%lu\n", pagesz);
        } else {
            vms_printf("  FAIL: getauxval(AT_PAGESZ) returned 0\n");
            failures++;
        }
    }

    /* Test 6: errno mapping */
    {
        uint32_t st = vms_errno_to_status(0);
        if (st == SS$_NORMAL)
            vms_printf("  OK: errno 0 -> SS$_NORMAL\n");
        else {
            vms_printf("  FAIL: errno mapping\n");
            failures++;
        }

        st = vms_errno_to_status(VMS_ENOENT);
        if (st == SS$_NOSUCHFILE)
            vms_printf("  OK: ENOENT -> SS$_NOSUCHFILE\n");
        else {
            vms_printf("  FAIL: ENOENT mapping\n");
            failures++;
        }
    }

    if (failures == 0)
        vms_printf("All CRT tests passed.\n");
    else
        vms_printf("Some CRT tests FAILED (%d).\n", failures);

    return failures;
}
