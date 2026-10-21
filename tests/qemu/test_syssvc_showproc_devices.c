/*
 * test_syssvc_showproc_devices.c - SHOW PROCESS's "Devices allocated:" section,
 * read from the executive's device table (rd vms-c31).
 *
 * What OpenVMS VAX V7.3 printed (docs/oracle/vax73-show-system-process.md
 * Section 2.3, captured on VAX1 2026-10-21): after "Default file spec:", a blank
 * line, then
 *
 *     Devices allocated:  VAX1$OPA0:
 *                         VAX1$LPA0:
 *                         VAX1$TTD3:
 *
 * -- the label in a 20-column field, every further device alone on its line at
 * column 20 -- and no section at all for a process that owns nothing (Section
 * 3.1). OVMX's full device name carries no "node$" prefix (the vms-dv1
 * remainder $GETDVI shares), so the names asserted here are the executive's own.
 *
 * A-WRITES / B-READS: THIS process allocates the devices ($ALLOC, straight to
 * the executive) and a DIFFERENT process -- the shipped DCL.EXE -- reports them
 * with SHOW PROCESS/IDENTIFICATION=<this pid>. DCL can only learn them from the
 * executive's device-owner rows. After $DALLOC the section must be gone.
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

#include "starlet.h"
#include "ssdef.h"
#include "jpidef.h"
#include "dcdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"
#define LABEL_WIDTH 20   /* oracle Section 2.1: body labels sit in 20 columns */

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

/* Copy line `n` after the line that starts with `prefix` (0 = that line) into out. */
static int line_after(const char *text, const char *prefix, int n, char *out, size_t outsz)
{
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, prefix, strlen(prefix)) == 0)
            break;
        p = strchr(p, '\n');
        if (p) p++;
    }
    if (!p || !*p) return -1;
    while (n-- > 0) {
        p = strchr(p, '\n');
        if (!p) return -1;
        p++;
    }
    size_t len = strcspn(p, "\n");
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* Is the line just before the one starting with `prefix` empty? */
static int blank_line_before(const char *text, const char *prefix)
{
    const char *hit = strstr(text, prefix);
    if (!hit || hit - text < 2 || hit[-1] != '\n')
        return 0;
    return hit[-2] == '\n';
}

int main(void)
{
    static char out[16384];
    static char script[256];
    char want[128], got[128];

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_showproc_devices (SHOW PROCESS Devices allocated: from the executive) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_showproc_devices: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    uint32_t self = 0;
    uint16_t rl = 0;
    struct item_list_3 it[2];
    memset(it, 0, sizeof it);
    it[0].buflen = sizeof self;
    it[0].item_code = JPI$_PID;
    it[0].bufaddr = &self;
    it[0].retlen = &rl;
    CHECK((sys$getjpi(0, NULL, NULL, it, NULL, NULL, 0) & 1) && self != 0,
          "$GETJPI JPI$_PID names this process");
    snprintf(script, sizeof script, "SHOW PROCESS/IDENTIFICATION=%08X\n", self);

    /* 0. Owning nothing: no section at all (oracle Section 3.1). */
    CHECK(run_dcl(script, out, sizeof out) == 0, "DCL ran SHOW PROCESS (nothing owned)");
    printf("  ---- nothing owned ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "Default file spec:") != NULL, "SHOW PROCESS printed the target's body");
    CHECK(strstr(out, "Devices allocated:") == NULL,
          "a process that owns no device has no Devices allocated: section");

    /* 1. Allocate OPA0: and one more device the executive will give us. */
    CHECK(vms_kif_alloc("OPA0:") & 1, "this process allocates OPA0: ($ALLOC)");
    char second[VMS_DEVNAM_SIZE] = "";
    {
        struct vms_devinfo info;
        uint32_t idx = 0;
        while (second[0] == '\0' && vms_kif_devscan(&idx, &info) == SS$_NORMAL) {
            info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
            /* Never a disk: a volume other suites mount is not ours to hold. */
            if (strcmp(info.devnam, "OPA0:") == 0 || info.owner_pid != 0 ||
                info.devclass == DC$_DISK)
                continue;
            if (vms_kif_alloc(info.devnam) & 1)
                snprintf(second, sizeof second, "%s", info.devnam);
        }
    }
    CHECK(second[0] != '\0', "the executive allocated a second device to this process");
    printf("  second device: %s\n", second);

    /* The order VMS walks is the I/O database's; OVMX's is the device table's. */
    char first_dev[VMS_DEVNAM_SIZE] = "", next_dev[VMS_DEVNAM_SIZE] = "";
    {
        struct vms_devinfo info;
        uint32_t idx = 0;
        while (vms_kif_devscan(&idx, &info) == SS$_NORMAL) {
            info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
            if (info.owner_pid != self) continue;
            if (!first_dev[0]) snprintf(first_dev, sizeof first_dev, "%s", info.devnam);
            else if (!next_dev[0]) snprintf(next_dev, sizeof next_dev, "%s", info.devnam);
        }
    }

    CHECK(run_dcl(script, out, sizeof out) == 0, "DCL ran SHOW PROCESS (two devices owned)");
    printf("  ---- two devices ----\n%s  ---- end ----\n", out);
    snprintf(want, sizeof want, "%-*s%s", LABEL_WIDTH, "Devices allocated:", first_dev);
    /* negctl: dcl-showproc-devices-unread */
    CHECK(line_after(out, "Devices allocated:", 0, got, sizeof got) == 0 && strcmp(got, want) == 0,
          "Devices allocated: names the first owned device at column 20");
    snprintf(want, sizeof want, "%-*s%s", LABEL_WIDTH, "", next_dev);
    /* negctl: dcl-showproc-devices-unread */
    CHECK(line_after(out, "Devices allocated:", 1, got, sizeof got) == 0 && strcmp(got, want) == 0,
          "the second owned device sits alone on the next line at column 20");
    CHECK(blank_line_before(out, "Devices allocated:"),
          "a blank line precedes Devices allocated: (oracle Section 2.3)");
    CHECK(strstr(out, "OPA0:") != NULL && strstr(out, second) != NULL,
          "both allocated devices are named");

    /* 2. Give them back: the section goes. */
    CHECK(vms_kif_dalloc("OPA0:") & 1, "$DALLOC OPA0:");
    if (second[0]) CHECK(vms_kif_dalloc(second) & 1, "$DALLOC the second device");
    CHECK(run_dcl(script, out, sizeof out) == 0, "DCL ran SHOW PROCESS (after $DALLOC)");
    printf("  ---- after $DALLOC ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "Devices allocated:") == NULL,
          "after $DALLOC the Devices allocated: section is gone");

    printf("=== test_syssvc_showproc_devices: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
