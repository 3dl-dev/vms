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

    if (failures == 0)
        vms_printf("All stdio tests passed.\n");
    else
        vms_printf("Some stdio tests FAILED (%d).\n", failures);

    return failures;
}
