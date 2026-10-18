/*
 * test_read_over_acp (vms-5303e): rms_read_over_acp is fail-honest.
 *
 * DCL DUMP reads a file's bytes through the Files-11 ACP with it. With no
 * executive (this host test) the ACP is unreachable and the service must say so
 * -- RMS$_ACC, no buffer -- even when a file of that very name exists on the
 * host's own file system: it never reads a POSIX path as a stand-in for the
 * volume (INV-6). With an executive present, a name that is not on any volume
 * is never a success either.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rms/rms.h"
#include "rmsdef.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

int main(void)
{
    uint8_t *buf = (uint8_t *)0x1;
    uint64_t len = 99;

    check(rms_read_over_acp(NULL, &buf, &len) == RMS$_FAB, "a null spec is RMS$_FAB");
    check(rms_read_over_acp("SYS$DISK:[000000]X.DAT", NULL, &len) == RMS$_FAB, "a null buffer slot is RMS$_FAB");

    /* A file that exists on the HOST file system under the very name asked for. */
    char path[] = "/tmp/ovmx_read_over_acp_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "a host file to be (wrongly) read exists");
    if (fd >= 0) {
        const char data[] = "must not come back";
        if (write(fd, data, sizeof data) != (ssize_t)sizeof data)
            fails++;
        close(fd);
    }
    buf = (uint8_t *)0x1;
    len = 99;
    uint32_t st = rms_read_over_acp(path, &buf, &len);
    check(st != RMS$_NORMAL, "a host path is never read as if it were on the volume");
    check(buf == NULL && len == 0, "a failed read leaves no buffer and no length");
    if (rms_executive_absent())
        check(st == RMS$_ACC, "no executive: RMS$_ACC (the ACP is unreachable)");
    unlink(path);
    free(buf);

    printf("%s\n", fails ? "test_read_over_acp: FAILED" : "test_read_over_acp: OK");
    return fails ? 1 : 0;
}
