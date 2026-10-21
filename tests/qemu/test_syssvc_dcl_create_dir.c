/*
 * test_syssvc_dcl_create_dir.c - CREATE/DIRECTORY's messages and conditions are
 * VMS's own (rd vms-916 follow-up), driven through the shipped DCL.EXE against
 * the executive's Files-11 ACP on the writable VDA0: volume.
 *
 * What OpenVMS VAX V7.3 printed (VAX1, 2026-10-21,
 * tests/lab/captures/create-directory-20261021/vax1-create-directory.txt):
 *   a new directory               -> nothing at all
 *   the same directory again      -> "%CREATE-I-EXISTS, [.OVMXB10] already exists",
 *                                    $STATUS %X10911293
 *   on a device that is not there -> "%CREATE-E-DIRNOTCRE, NOSUCHDEV:[OVMXB10]
 *                                    directory file not created" and a secondary
 *                                    "-SYSTEM-W-NOSUCHDEV, ..." line, $STATUS %X109112C2
 * OVMX's $STATUS carries no inhibit-display bit (0x10000000) for any command yet,
 * so the values asserted are %X00911293 / %X009112C2. Before this, OVMX printed
 * an invented "%DCL-I-CREATED, directory already exists" and an invented
 * "%RMS-E-CRE, cannot create directory" with SS$_FILACCERR.
 *
 * No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "stsdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define OVMXDIR_FID_NUM 11u   /* VDA0:[OVMXDIR], as test_syssvc_create_dir.c */

/* Remove what the probe created, straight through the ACP. */
static uint32_t delete_in(uint32_t chan, uint16_t did, const char *name)
{
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_DELETE;
    f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = did;
    f.did_seq = 1;
    f.version = 1;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(&f);
}

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];
    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) return -1;
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* best effort */ }
    close(in_pipe[1]);
    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

int main(void)
{
    static char out[16384];

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_create_dir (CREATE/DIRECTORY: VMS's messages and conditions) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_create_dir: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    uint32_t chan = 0;
    (void)vms_kif_acp_mount("VDA0:");   /* already mounted by an earlier suite is fine */
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_assign("VDA0:", &chan)) && chan != 0,
          "$ASSIGN a file-class channel to VDA0:");

    CHECK(run_dcl("CREATE/DIRECTORY VDA0:[OVMXDIR.CRB10]\n"
                  "SHOW SYMBOL $STATUS\n"
                  "WRITE SYS$OUTPUT \"--AGAIN--\"\n"
                  "CREATE/DIRECTORY VDA0:[OVMXDIR.CRB10]\n"
                  "SHOW SYMBOL $STATUS\n"
                  "WRITE SYS$OUTPUT \"--NODEV--\"\n"
                  "CREATE/DIRECTORY NOSUCHDEV:[CRB10]\n"
                  "SHOW SYMBOL $STATUS\n", out, sizeof out) == 0,
          "DCL ran the CREATE/DIRECTORY probes");
    printf("  ---- CREATE/DIRECTORY probes ----\n%s  ---- end ----\n", out);

    const char *again = strstr(out, "--AGAIN--");
    const char *nodev = strstr(out, "--NODEV--");
    CHECK(again && nodev, "the probe markers are present");
    if (!again || !nodev) {
        printf("=== test_syssvc_dcl_create_dir: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    {
        char first[4096];
        size_t n = (size_t)(again - out);
        if (n >= sizeof first) n = sizeof first - 1;
        memcpy(first, out, n); first[n] = '\0';
        CHECK(strstr(first, "%CREATE-") == NULL && strstr(first, "%DCL-") == NULL &&
              strstr(first, "%RMS-") == NULL,
              "a new directory is created silently, as VMS does");
        CHECK(strstr(first, "$STATUS = \"%X00000001\"") != NULL,
              "the new directory leaves $STATUS = %X00000001");
    }
    {
        char second[4096];
        size_t n = (size_t)(nodev - again);
        if (n >= sizeof second) n = sizeof second - 1;
        memcpy(second, again, n); second[n] = '\0';
        /* negctl: dcl-create-dir-invented-msgs */
        CHECK(strstr(second, "%CREATE-I-EXISTS, VDA0:[OVMXDIR.CRB10] already exists") != NULL,
              "an existing directory is %CREATE-I-EXISTS, <spec> already exists (oracle)");
        /* negctl: dcl-create-dir-invented-msgs */
        CHECK(strstr(second, "%X00911293") != NULL,
              "an existing directory leaves $STATUS = %X00911293 (CREATE-I-EXISTS)");
    }
    /* negctl: dcl-create-dir-invented-msgs */
    CHECK(strstr(nodev, "%CREATE-E-DIRNOTCRE, NOSUCHDEV:[CRB10] directory file not created") != NULL,
          "a failed create is %CREATE-E-DIRNOTCRE, <spec> directory file not created (oracle)");
    /* negctl: dcl-create-dir-invented-msgs */
    CHECK(strstr(nodev, "%X009112C2") != NULL,
          "a failed create leaves $STATUS = %X009112C2 (CREATE-E-DIRNOTCRE)");

    CHECK($VMS_STATUS_SUCCESS(delete_in(chan, OVMXDIR_FID_NUM, "CRB10.DIR")),
          "delete VDA0:[OVMXDIR]CRB10.DIR (restore)");

    printf("=== test_syssvc_dcl_create_dir: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
