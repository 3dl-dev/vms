/*
 * Hermetic test for dcl_memfile_open (rd vms-97c2): DCL stages a command
 * procedure in a stream that must NOT depend on the filesystem accepting
 * writes. On NetBSD a tmpfs write fails (ENOSPC) below 4 MB available memory;
 * here RLIMIT_FSIZE=0 makes every file write fail the same way (EFBIG), so the
 * test runs the staging under the condition that truncated STARTUP.COM on a
 * clustered 16 MB VAX.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include "dcl/dcl_memfile.h"

static int failures;
static struct rlimit saved;
/* Output is a file under ctest, and the limit under test refuses file writes:
 * lift it for each report line, then put it back. */
static void report(int ok, const char *msg)
{
    struct rlimit zero = { 0, saved.rlim_max };

    setrlimit(RLIMIT_FSIZE, &saved);
    printf("  %s: %s\n", ok ? "PASS" : "FAIL", msg);
    fflush(stdout);
    setrlimit(RLIMIT_FSIZE, &zero);
    if (!ok)
        failures++;
}
#define CHECK(c, msg) report((c) ? 1 : 0, msg)

int main(void)
{
    struct rlimit rl;
    char *buf;
    size_t len = 0, cap = 256 * 1024;
    int lines = 0;
    char line[256];
    struct dcl_pf *fp;
    FILE *tf;

    signal(SIGXFSZ, SIG_IGN);
    getrlimit(RLIMIT_FSIZE, &saved);
    rl.rlim_cur = 0;
    rl.rlim_max = saved.rlim_max;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "file writes are now refused (RLIMIT_FSIZE=0)");

    /* NEGATIVE CONTROL: the old staging (tmpfile + fwrite) really fails here,
     * so the assertions below can only pass for a stream that is not a file. */
    tf = tmpfile();
    if (tf) {
        size_t w = fwrite("$ EXIT\n", 1, 7, tf);
        int fl = fflush(tf);
        CHECK(w != 7 || fl != 0, "NEGCTL: a tmpfile() staging write fails under the same limit");
        fclose(tf);
    } else {
        CHECK(1, "NEGCTL: tmpfile() itself fails under the same limit");
    }

    buf = malloc(cap);
    for (int i = 0; len + 40 < cap; i++)
        len += (size_t)snprintf(buf + len, cap - len, "$ ! record %d of the procedure\n", i);
    fp = dcl_pf_from_mem(buf, len);
    CHECK(fp != NULL, "dcl_pf_from_mem succeeds with file writes refused");
    if (!fp)
        return 1;
    while (dcl_pf_gets(line, sizeof line, fp))
        lines++;
    CHECK(lines > 5000, "every record reads back through fgets (no truncation)");
    CHECK(dcl_pf_gets(line, sizeof line, fp) == NULL, "the stream ends at a clean EOF");
    CHECK(dcl_pf_seek(fp, 0, SEEK_SET) == 0 && dcl_pf_gets(line, sizeof line, fp) &&
          strcmp(line, "$ ! record 0 of the procedure\n") == 0, "fseek(0) rewinds to the first record");
    long mark = 0;
    dcl_pf_seek(fp, 100, SEEK_SET);
    dcl_pf_gets(line, sizeof line, fp);    /* partial line, then a whole one */
    mark = dcl_pf_tell(fp);
    dcl_pf_gets(line, sizeof line, fp);
    CHECK(dcl_pf_seek(fp, mark, SEEK_SET) == 0, "GOSUB-style resume: seek to a recorded tell()");
    char again[256];
    CHECK(dcl_pf_gets(again, sizeof again, fp) && strcmp(again, line) == 0, "the resumed read returns the same record");
    CHECK(dcl_pf_seek(fp, 0, SEEK_END) == 0 && dcl_pf_gets(line, sizeof line, fp) == NULL, "SEEK_END then read is EOF");
    CHECK(dcl_pf_close(fp) == 0, "close frees the buffer it owns");

    fp = dcl_pf_from_mem(NULL, 0);
    CHECK(fp != NULL && dcl_pf_gets(line, sizeof line, fp) == NULL, "an empty procedure is an empty stream");
    if (fp)
        dcl_pf_close(fp);

    /* ---- rd vms-f47: the other tmpfs users, under the same refusal ---------- */
    CHECK(dcl_scratch_tmpfile() == NULL,
          "the pager's scratch refuses to be created when it cannot take data (listing goes unpaged)");
    {
        int rc = dcl_write_all(open("/dev/full", O_WRONLY), "SPAWN-CMD\n", 10);
        CHECK(rc == -ENOSPC, "dcl_write_all reports a refused write (SPAWN's scratch command)");
        CHECK(dcl_write_all(open("/dev/null", O_WRONLY), "x", 1) == 0, "dcl_write_all succeeds when the write does");
    }
    {
        struct dcl_sysinput si;
        char got[8192];
        ssize_t n;
        int keep = dup(0);
        char big[9000];

        si.saved_fd0 = -1;
        CHECK(dcl_sysinput_install(&si, NULL, 0) == 0, "an empty input block installs (no scratch file)");
        n = read(0, got, sizeof got);
        CHECK(n == 0, "... and reads as an honest EOF");
        dup2(si.saved_fd0, 0); close(si.saved_fd0);

        si.saved_fd0 = -1;
        CHECK(dcl_sysinput_install(&si, "ADD USER\nEXIT\n", 14) == 0, "a small block installs with file writes refused");
        n = read(0, got, sizeof got);
        CHECK(n == 14 && memcmp(got, "ADD USER\nEXIT\n", 14) == 0, "... and the image reads the whole block, not a truncation");
        n = read(0, got, sizeof got);
        CHECK(n == 0, "... then EOF");
        dup2(si.saved_fd0, 0); close(si.saved_fd0);

        memset(big, 'x', sizeof big);
        si.saved_fd0 = -1;
        CHECK(dcl_sysinput_install(&si, big, sizeof big) == -1 && si.saved_fd0 == -1,
              "a block too big for a pipe fails HONESTLY when the scratch file refuses it (fd 0 untouched)");
        CHECK(fcntl(0, F_GETFD) >= 0, "fd 0 is still open after the refusal");
        dup2(keep, 0); close(keep);
    }

    setrlimit(RLIMIT_FSIZE, &saved);
    printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
    return failures != 0;
}
