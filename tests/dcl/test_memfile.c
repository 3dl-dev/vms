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
    FILE *fp;

    signal(SIGXFSZ, SIG_IGN);
    getrlimit(RLIMIT_FSIZE, &saved);
    rl.rlim_cur = 0;
    rl.rlim_max = saved.rlim_max;
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0, "file writes are now refused (RLIMIT_FSIZE=0)");

    /* NEGATIVE CONTROL: the old staging (tmpfile + fwrite) really fails here,
     * so the assertions below can only pass for a stream that is not a file. */
    fp = tmpfile();
    if (fp) {
        size_t w = fwrite("$ EXIT\n", 1, 7, fp);
        int fl = fflush(fp);
        CHECK(w != 7 || fl != 0, "NEGCTL: a tmpfile() staging write fails under the same limit");
        fclose(fp);
    } else {
        CHECK(1, "NEGCTL: tmpfile() itself fails under the same limit");
    }

    buf = malloc(cap);
    for (int i = 0; len + 40 < cap; i++)
        len += (size_t)snprintf(buf + len, cap - len, "$ ! record %d of the procedure\n", i);
    fp = dcl_memfile_open(buf, len);
    CHECK(fp != NULL, "dcl_memfile_open succeeds with file writes refused");
    if (!fp)
        return 1;
    while (fgets(line, sizeof line, fp))
        lines++;
    CHECK(lines > 5000, "every record reads back through fgets (no truncation)");
    CHECK(feof(fp) && !ferror(fp), "the stream ends at a clean EOF");
    CHECK(fseek(fp, 0, SEEK_SET) == 0 && fgets(line, sizeof line, fp) &&
          strcmp(line, "$ ! record 0 of the procedure\n") == 0, "fseek(0) rewinds to the first record");
    long mark = 0;
    fseek(fp, 100, SEEK_SET);
    fgets(line, sizeof line, fp);          /* partial line, then a whole one */
    mark = ftell(fp);
    fgets(line, sizeof line, fp);
    CHECK(fseek(fp, mark, SEEK_SET) == 0, "GOSUB-style resume: fseek to a recorded ftell()");
    char again[256];
    CHECK(fgets(again, sizeof again, fp) && strcmp(again, line) == 0, "the resumed read returns the same record");
    CHECK(fseek(fp, 0, SEEK_END) == 0 && fgets(line, sizeof line, fp) == NULL, "SEEK_END then read is EOF");
    CHECK(fclose(fp) == 0, "fclose frees the buffer it owns");

    fp = dcl_memfile_open(NULL, 0);
    CHECK(fp != NULL && fgets(line, sizeof line, fp) == NULL, "an empty procedure is an empty stream");
    if (fp)
        fclose(fp);

    setrlimit(RLIMIT_FSIZE, &saved);
    printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
    return failures != 0;
}
