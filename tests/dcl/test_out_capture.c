/*
 * Hermetic test for the /PAGE capture of DCL's output sink (rd vms-07ae):
 * while the pager gathers SYS$OUTPUT, each record DCL writes reaches the
 * pager whole (no '\n', a long line as several records), the unended last
 * line reaches it when gathering stops, SYS$ERROR is not gathered, a record
 * the pager refuses sets the stream's error, and nothing gathered reaches the
 * process's fd 1.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dcl/dcl_out.h"

static int failures;
#define CHECK(c, msg) do { int ok_ = (c) ? 1 : 0; \
    fprintf(stderr, "  %s: %s\n", ok_ ? "PASS" : "FAIL", msg); \
    if (!ok_) failures++; } while (0)

struct got { char rec[16][5000]; size_t n[16]; int count; int refuse; };

static int take(void *arg, const char *rec, size_t n, int open)
{
    struct got *g = arg;
    if (open || g->refuse || g->count == 16)
        return -1;                    /* a pager takes whole records only */
    memcpy(g->rec[g->count], rec, n);
    g->n[g->count++] = n;
    return 0;
}

static int is(const struct got *g, int i, const char *s)
{
    return i < g->count && g->n[i] == strlen(s) && memcmp(g->rec[i], s, g->n[i]) == 0;
}

int main(void)
{
    static struct got g;
    static char longline[4096 + 10 + 1];
    char fdbuf[64];
    int p[2];

    /* fd 1 is a pipe the test reads: the gathered output must not reach it */
    if (pipe(p) != 0 || dup2(p[1], STDOUT_FILENO) < 0)
        return 2;
    close(p[1]);

    dcl_out_capture(take, &g);
    dcl_out_printf("Directory DKA0:[USER]\n");
    dcl_out_puts("");
    dcl_out_fputs("A.TXT;1");
    dcl_out_fputs("    1\n");
    memset(longline, 'X', sizeof longline - 1);
    dcl_out_puts(longline);
    dcl_err_printf("%%DCL-W-SOMETHING, not gathered\n");
    dcl_out_fputs("Total of 1 file.");          /* unended */
    dcl_out_flush();
    CHECK(g.count == 5, "the ended records are gathered as they are written");
    dcl_out_capture(NULL, NULL);
    CHECK(g.count == 6, "the unended last line is gathered when gathering stops");
    CHECK(is(&g, 0, "Directory DKA0:[USER]"), "a record arrives without its newline");
    CHECK(is(&g, 1, ""), "an empty record is a record");
    CHECK(is(&g, 2, "A.TXT;1    1"), "pieces of one line arrive as one record");
    CHECK(g.n[3] == 4096 && g.n[4] == 10, "a long line arrives as whole records");
    CHECK(is(&g, 5, "Total of 1 file."), "the last line arrives");
    int err_seen = 0;
    for (int i = 0; i < g.count; i++)
        err_seen |= memmem(g.rec[i], g.n[i], "SOMETHING", 9) != NULL;
    CHECK(!err_seen, "SYS$ERROR is not gathered");
    CHECK(dcl_out_error() == 0, "no error once gathering stops");

    memset(&g, 0, sizeof g);
    g.refuse = 1;
    dcl_out_capture(take, &g);
    dcl_out_puts("refused");
    CHECK(dcl_out_error() != 0, "a record the pager refuses sets the stream error");
    dcl_out_capture(NULL, NULL);
    CHECK(dcl_out_error() == 0, "the error is the pager's, not the stream's after");

    /* after gathering, output goes to the stream again */
    dcl_out_puts("after");
    dcl_out_flush();
    close(STDOUT_FILENO);
    ssize_t r = read(p[0], fdbuf, sizeof fdbuf - 1);
    fdbuf[r > 0 ? r : 0] = '\0';
    CHECK(strcmp(fdbuf, "after\n") == 0,
          "fd 1 carries only what was written outside the gathering");

    fprintf(stderr, failures ? "OUT_CAPTURE_FAIL (%d)\n" : "OUT_CAPTURE_OK\n", failures);
    return failures ? 1 : 0;
}
