/*
 * corpus_rt_forcex_stdio.c - the DEC C stdio->RMS mapping, for ONE corpus program (vms-ccc).
 *
 * The corpus runtime programs are plain musl-static images, so their fopen() is a Linux open()
 * of the literal string "sys$scratch:demo_forcex.com;" in the current directory. On OpenVMS the
 * same fopen() is RMS: it creates SYS$SCRATCH:DEMO_FORCEX.COM on the system disk, which is where
 * LIB$SPAWN (INPUT=<that spec>) reads it back. sys_forcex writes a command procedure with fopen/
 * fprintf/fclose and hands the SAME spec to LIB$SPAWN, so under a Linux fopen the two never meet.
 *
 * This shim (linked into corpus_rt_sys_forcex only, via ld --wrap) gives that one program the DEC C
 * behaviour: a "w" stream is an in-memory stream; fclose() lays its records down through RMS
 * ($CREATE + $PUT over the ACP) at the spec the program named. Any other open falls through to the
 * real fopen. The program text is untouched; this is harness wiring, not product code.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rms_textfile.h"

extern FILE *__real_fopen(const char *, const char *);
extern int __real_fclose(FILE *);

#define MAXOPEN 4
static struct { FILE *fp; char *buf; size_t len; char path[512]; } open_w[MAXOPEN];

FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (!path || !mode || mode[0] != 'w' || !strchr(path, ':'))
        return __real_fopen(path, mode);          /* a Linux path: leave it alone */
    for (int i = 0; i < MAXOPEN; i++) {
        if (open_w[i].fp) continue;
        open_w[i].buf = NULL; open_w[i].len = 0;
        open_w[i].fp = open_memstream(&open_w[i].buf, &open_w[i].len);
        if (!open_w[i].fp) return NULL;
        snprintf(open_w[i].path, sizeof open_w[i].path, "%s", path);
        return open_w[i].fp;
    }
    return NULL;
}

int __wrap_fclose(FILE *fp)
{
    for (int i = 0; i < MAXOPEN; i++) {
        if (open_w[i].fp != fp) continue;
        int rc = __real_fclose(fp);                /* finalises buf/len */
        int ok = (rc == 0);
        int first = 1;
        char *save = NULL;
        for (char *ln = strtok_r(open_w[i].buf, "\n", &save); ok && ln; ln = strtok_r(NULL, "\n", &save)) {
            ok = (first ? rms_textfile_write_line(open_w[i].path, ln)
                        : rms_textfile_append_line(open_w[i].path, ln)) == 0;
            if (!ok) { fprintf(stderr, "SHIMDBG: RMS write of [%s] to '%s' failed (first=%d)\n", ln, open_w[i].path, first);
                       fprintf(stderr, "SHIMDBG: control write SYS$SYSROOT:[SYSMGR]SHIMCTL.TMP -> %d\n", rms_textfile_write_line("SYS$SYSROOT:[SYSMGR]SHIMCTL.TMP", "x"));
                       fprintf(stderr, "SHIMDBG: control write sys$scratch:shimctl.tmp -> %d\n", rms_textfile_write_line("sys$scratch:shimctl.tmp", "x")); }
            first = 0;
        }
        free(open_w[i].buf);
        open_w[i].fp = NULL;
        return ok ? 0 : EOF;
    }
    return __real_fclose(fp);
}
