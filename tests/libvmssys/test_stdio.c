/*
 * test_stdio.c - Test buffered I/O layer
 */

#include "vmssys.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    int failures = 0;

    vms_printf("=== libvmssys stdio test ===\n");

    /* Test 1: vms_printf (if we see this, it works) */
    vms_printf("  OK: vms_printf\n");

    /* Test 2: vms_fprintf to stderr */
    vms_fprintf(vms_stderr, "  OK: vms_fprintf to stderr\n");

    /* Test 3: Write and read back a temp file */
    {
        const char *tmppath = "/tmp/vmssys_test_stdio.tmp";
        vms_file_t *f = vms_fopen(tmppath, "w");
        if (f) {
            vms_fprintf(f, "line1\nline2\nline3\n");
            vms_fclose(f);

            f = vms_fopen(tmppath, "r");
            if (f) {
                char line[64];
                int ok = 1;

                if (vms_fgets(line, sizeof(line), f)) {
                    if (vms_strcmp(line, "line1\n") != 0) ok = 0;
                } else ok = 0;

                if (vms_fgets(line, sizeof(line), f)) {
                    if (vms_strcmp(line, "line2\n") != 0) ok = 0;
                } else ok = 0;

                if (vms_fgets(line, sizeof(line), f)) {
                    if (vms_strcmp(line, "line3\n") != 0) ok = 0;
                } else ok = 0;

                /* Should be EOF now */
                if (vms_fgets(line, sizeof(line), f) != NULL) ok = 0;

                vms_fclose(f);

                if (ok)
                    vms_printf("  OK: file write/read cycle\n");
                else {
                    vms_printf("  FAIL: file write/read cycle\n");
                    failures++;
                }
            } else {
                vms_printf("  FAIL: fopen for read\n");
                failures++;
            }

            /* Clean up */
            vms_sys_unlinkat(VMS_AT_FDCWD, tmppath, 0);
        } else {
            vms_printf("  FAIL: fopen for write\n");
            failures++;
        }
    }

    /* Test 4: fread/fwrite binary */
    {
        const char *tmppath = "/tmp/vmssys_test_stdio2.tmp";
        vms_file_t *f = vms_fopen(tmppath, "w");
        if (f) {
            unsigned char data[256];
            for (int i = 0; i < 256; i++)
                data[i] = (unsigned char)i;
            vms_fwrite(data, 1, 256, f);
            vms_fclose(f);

            f = vms_fopen(tmppath, "r");
            if (f) {
                unsigned char rbuf[256];
                vms_size_t n = vms_fread(rbuf, 1, 256, f);
                vms_fclose(f);

                if (n == 256 && vms_memcmp(data, rbuf, 256) == 0)
                    vms_printf("  OK: binary fwrite/fread\n");
                else {
                    vms_printf("  FAIL: binary fwrite/fread (read %lu bytes)\n", n);
                    failures++;
                }
            } else {
                vms_printf("  FAIL: fopen binary read\n");
                failures++;
            }

            vms_sys_unlinkat(VMS_AT_FDCWD, tmppath, 0);
        } else {
            vms_printf("  FAIL: fopen binary write\n");
            failures++;
        }
    }

    /* Test 5: fseek/ftell */
    {
        const char *tmppath = "/tmp/vmssys_test_stdio3.tmp";
        vms_file_t *f = vms_fopen(tmppath, "w");
        if (f) {
            vms_fputs("0123456789", f);
            vms_fclose(f);

            f = vms_fopen(tmppath, "r");
            if (f) {
                vms_fseek(f, 5, VMS_SEEK_SET);
                char buf[8];
                vms_fgets(buf, sizeof(buf), f);
                int ok = (vms_strcmp(buf, "56789") == 0);
                vms_fclose(f);
                if (ok)
                    vms_printf("  OK: fseek/fgets\n");
                else {
                    vms_printf("  FAIL: fseek/fgets (got \"%s\")\n", buf);
                    failures++;
                }
            }
            vms_sys_unlinkat(VMS_AT_FDCWD, tmppath, 0);
        }
    }

    /* Test 6: fwrite buffer-boundary correctness (vms-888).
     * Drive vms_fwrite across the internal buffer boundary and read the bytes
     * back to confirm the output is complete and correct for exactly BUFSZ,
     * BUFSZ+1, and a large multi-flush write. */
    {
        const char *tmppath = "/tmp/vmssys_test_stdio_bnd.tmp";
        static unsigned char big[3 * VMS_STDIO_BUFSZ + 123];
        static unsigned char rback[sizeof(big)];
        for (unsigned i = 0; i < sizeof(big); i++)
            big[i] = (unsigned char)(i * 31u + 7u); /* non-trivial pattern */

        vms_size_t cases[3] = {
            VMS_STDIO_BUFSZ,        /* exact buffer fill */
            VMS_STDIO_BUFSZ + 1,    /* one past the boundary */
            sizeof(big)             /* many flushes */
        };
        int ok = 1;
        for (int c = 0; c < 3; c++) {
            vms_size_t n = cases[c];
            vms_file_t *f = vms_fopen(tmppath, "w");
            if (!f) { ok = 0; break; }
            vms_size_t w = vms_fwrite(big, 1, n, f);
            if (w != n) ok = 0;               /* full count on success */
            vms_fclose(f);

            f = vms_fopen(tmppath, "r");
            if (!f) { ok = 0; break; }
            vms_size_t r = vms_fread(rback, 1, n, f);
            vms_fclose(f);
            if (r != n) ok = 0;
            for (vms_size_t i = 0; i < n && ok; i++)
                if (rback[i] != big[i]) ok = 0;
        }
        vms_sys_unlinkat(VMS_AT_FDCWD, tmppath, 0);
        if (ok)
            vms_printf("  OK: fwrite buffer-boundary correctness\n");
        else {
            vms_printf("  FAIL: fwrite buffer-boundary correctness\n");
            failures++;
        }
    }

    /* Test 7: fwrite off-by-one overflow + honest error return (vms-888).
     * A flush failure leaves buf_pos == VMS_STDIO_BUFSZ. On origin/main the
     * write-then-check loop then (a) returned i/size, falsely claiming ~BUFSZ
     * items were written though the failing flush persisted nothing, and (b)
     * on the next reuse of the handle wrote buf[VMS_STDIO_BUFSZ] out of bounds.
     * We force the failure with fd = -1 (writes always fail) and assert the
     * fix: honest 0 return and buf_pos never left out of range. */
    {
        vms_file_t bad;
        vms_memset(&bad, 0, sizeof(bad));
        bad.fd = -1;                 /* every vms_sys_write returns -1 */
        bad.flags = VMS_FILE_WRITE;
        bad.buf_mode = 0;            /* fully buffered */

        static unsigned char src[VMS_STDIO_BUFSZ + 16];
        for (unsigned i = 0; i < sizeof(src); i++)
            src[i] = 0xAB;

        int ok = 1;
        /* First write overfills the buffer; the flush fails. Nothing reached
         * the fd, so the honest return is 0 (origin/main returned ~BUFSZ). */
        vms_size_t w1 = vms_fwrite(src, 1, VMS_STDIO_BUFSZ + 8, &bad);
        if (w1 != 0) ok = 0;                         /* return-value fix */
        if (bad.buf_pos < 0 || bad.buf_pos > VMS_STDIO_BUFSZ) ok = 0;

        /* Reuse the handle while buf_pos == BUFSZ. On origin/main the first
         * byte here is written to buf[BUFSZ] (OOB), corrupting buf_pos. The
         * fix flushes first, so buf_pos stays in range and the return is 0. */
        vms_size_t w2 = vms_fwrite(src, 1, 8, &bad);
        if (w2 != 0) ok = 0;
        if (bad.buf_pos < 0 || bad.buf_pos > VMS_STDIO_BUFSZ) ok = 0;

        if (ok)
            vms_printf("  OK: fwrite overflow guard + honest error return\n");
        else {
            vms_printf("  FAIL: fwrite overflow guard (w1=%d buf_pos=%d w2=%d)\n",
                       (int)w1, bad.buf_pos, (int)w2);
            failures++;
        }
    }

    if (failures == 0)
        vms_printf("All stdio tests passed.\n");
    else
        vms_printf("Some stdio tests FAILED (%d).\n", failures);

    return failures;
}
