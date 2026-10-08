/*
 * test_syssvc_crtl_static.c - a statically linked C image's file system is RMS
 * (vms-003b).
 *
 * On OpenVMS every C program's fopen() is RMS. A static OVMX image gets that
 * from the C RTL file layer over RMS (src/vmsrms/crtl_rms_fd.c) reached through
 * the OVMX musl system-call funnel (src/crtl-musl-x86_64): once the image turns
 * the layer on, a file named by an RMS file specification is created, read,
 * stat()ed and deleted on the Files-11 volume, and no POSIX file appears.
 *
 * On the writable real-VAX fixture (VDA0:):
 *   - fopen("VDA0:[OVMXDIR]CRTLFD.TXT", "w") + fputs + fclose succeeds;
 *   - the executive ACP finds CRTLFD.TXT in [OVMXDIR] (File ID by name);
 *   - fopen(..., "r") + fgets reads the two lines back;
 *   - stat() reports the byte count written;
 *   - unlink() removes it, and the ACP no longer finds it;
 *   - with the layer off again, no POSIX file of that name exists in the cwd.
 * Without the layer linked (a host libc): honest SKIP (77). No /dev/vms: SKIP.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define UNIT "VDA0:"
#define OVMXDIR_FID_NUM 11u
#define SPEC "VDA0:[OVMXDIR]CRTLFD.TXT"

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

#if defined(OVMX_CRTLFD_STATIC)
void ovmx_crtl_fd_install(void);
extern long long (*__ovmx_sys_hook)(long long, long long, long long, long long,
                                    long long, long long, long long, int *);

/* Is CRTLFD.TXT in [OVMXDIR] on the volume? (the ACP's own lookup by name) */
static int on_volume(uint32_t chan)
{
    struct vms_acp_access_args a;
    uint32_t st;

    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = OVMXDIR_FID_NUM; a.did_seq = 1;
    a.version = 0;
    strncpy(a.name, "CRTLFD.TXT", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    if (st & 1)
        (void)vms_kif_acp_deaccess(chan);
    return (st & 1) != 0;
}
#endif

int main(void)
{
    printf("=== test_syssvc_crtl_static: the C RTL's file system is RMS ===\n");
#if !defined(OVMX_CRTLFD_STATIC)
    printf("=== test_syssvc_crtl_static: 0 passed, 0 failed (SKIPPED: the C RTL file layer is not linked in this build) ===\n");
    return EXIT_SKIP;
#else
    {
        static const char l1[] = "first line\n", l2[] = "the second line\n";
        uint32_t chan = 0;
        FILE *f;
        char buf[64];
        struct stat st;
        int ok;

        if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || vms_kif_open() < 0) {
            printf("=== test_syssvc_crtl_static: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
            return EXIT_SKIP;
        }
        check((vms_kif_acp_mount(UNIT) & 1) && (vms_kif_acp_assign(UNIT, &chan) & 1),
              "$MOUNT + $ASSIGN the writable ODS-2 " UNIT);
        ovmx_crtl_fd_install();

        f = fopen(SPEC, "w");
        ok = f && fputs(l1, f) >= 0 && fputs(l2, f) >= 0;
        ok = (f && fclose(f) == 0) && ok;
        check(ok, "fopen(\"" SPEC "\", \"w\") + fputs + fclose");
        /* negctl: crtlfd-open-falls-to-posix */
        check(on_volume(chan), "the ACP finds CRTLFD.TXT in [OVMXDIR] on the volume");

        f = fopen(SPEC, "r");
        ok = f && fgets(buf, sizeof(buf), f) && strcmp(buf, l1) == 0 &&
             fgets(buf, sizeof(buf), f) && strcmp(buf, l2) == 0;
        if (f)
            fclose(f);
        check(ok, "fopen(..., \"r\") + fgets reads both lines back");

        memset(&st, 0, sizeof(st));
        check(stat(SPEC, &st) == 0 && st.st_size == (off_t)(sizeof(l1) - 1 + sizeof(l2) - 1),
              "stat() reports the byte count written");

        check(unlink(SPEC) == 0, "unlink() deletes it");
        check(!on_volume(chan), "...and the ACP no longer finds it");

        __ovmx_sys_hook = 0;                /* the kernel's file system again */
        check(access(SPEC, F_OK) != 0, "no POSIX file of that name was ever made in the cwd");

        (void)vms_kif_dassgn(chan);
        (void)vms_kif_acp_dmount(UNIT);
        printf("=== test_syssvc_crtl_static: %d passed, %d failed ===\n", pass, fail);
        return fail ? 1 : 0;
    }
#endif
}
