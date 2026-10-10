/*
 * test_syssvc_creprc_detached_lnm.c - a DETACHED $CREPRC'd process starts with
 * none of anyone's process logical names (rd vms-fe7).
 *
 * On OpenVMS a process created by $CREPRC starts with an empty LNM$PROCESS
 * table of its own; the creator's process names are not copied into it (only a
 * SPAWNed subprocess is given copies, and OVMX's subprocess path is a separate
 * registration). OVMX's executive gives a copy of the real parent's
 * LNM$PROCESS names to a process it registers on its own, because that is what
 * a fork()ed child inherits. A detached process is created by a short-lived
 * intermediate: if the intermediate had already exited when the new process
 * entered the executive's table, the new process had been reparented to the
 * nearest subreaper (PID 1 on a booted system), and took ITS names. On the
 * booted system that is how JOB_CONTROL, and through it every login, came up
 * with PID 1's plain SYS$INPUT/SYS$OUTPUT/SYS$COMMAND = "TT:" (7 of 16 boots;
 * the semantic oracle's LNM.TRN.SYSIN/SYSOUT/SYSCMD).
 *
 * The scenario here is that shape, made observable on the test rig:
 *   - a CREATOR process with no user name of its own (so $CREPRC takes the
 *     un-ticketed detached path, as the boot's startup DCL did) and marked a
 *     child subreaper (so an orphaned grandchild is reparented to IT, the
 *     analogue of PID 1),
 *   - defines FE7$MARK in its own LNM$PROCESS,
 *   - and creates ITERATIONS detached processes running this same binary in
 *     subject mode, each of which translates FE7$MARK in its own LNM$PROCESS
 *     and records what it found.
 * Every subject must find nothing. Before the fix the intermediate exited at
 * once, the race went the wrong way for a large share of creations, and a
 * subject reported the creator's value.
 *
 * negctl: creprc-detached-intermediate-not-held
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdint.h>
#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "prcdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/logical.h"

#define EXIT_SKIP   77
#define ITERATIONS  20
#define MARK_NAME   "FE7$MARK"
#define MARK_VALUE  "CREATOR-ONLY"
#define SUBJECT     "/tmp/fe7subject"     /* symlink to this binary */
#define OUTDIR      "/tmp/fe7out"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static uint32_t def(const char *table, const char *name, const char *val)
{
    struct dsc$descriptor_s td = mkdsc(table), nd = mkdsc(name);
    struct item_list_3 il[2];
    memset(il, 0, sizeof(il));
    il[0].buflen    = (uint16_t)strlen(val);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = (void *)val;
    return sys$crelnm(NULL, &td, &nd, NULL, il);
}

static uint32_t trn(const char *table, const char *name, char *out, size_t outsz)
{
    struct dsc$descriptor_s td = mkdsc(table), nd = mkdsc(name);
    struct item_list_3 il[2];
    uint16_t rl = 0;
    memset(il, 0, sizeof(il));
    il[0].buflen    = (uint16_t)(outsz - 1);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = out;
    il[0].retlen    = &rl;
    out[0] = '\0';
    uint32_t st = sys$trnlnm(NULL, &td, &nd, NULL, il);
    if (rl >= outsz) rl = (uint16_t)(outsz - 1);
    out[rl] = '\0';
    return st;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* SUBJECT MODE: what does MY LNM$PROCESS say about the creator's name? */
static int subject_main(void)
{
    char val[256], path[64];
    struct vms_procinfo me;
    memset(&me, 0, sizeof(me));
    uint32_t gst = vms_kif_getjpi_self(&me);
    uint32_t st = trn("LNM$PROCESS_TABLE", MARK_NAME, val, sizeof(val));
    snprintf(path, sizeof(path), OUTDIR "/%u", (unsigned)getpid());
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return 1;
    fprintf(f, "%s %08X %08X\n", (st & 1) ? val : "-", (unsigned)st,
            (gst & 1) ? (unsigned)me.vms_pid : 0u);
    fclose(f);
    rename(tmp, path);          /* the creator only ever reads a whole line */
    return 0;
}

static int count_results(int *copied, int *clean, char *first_copy, size_t fcsz)
{
    int n = 0;
    *copied = *clean = 0;
    DIR *d = opendir(OUTDIR);
    if (!d)
        return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char path[300], line[300];
        if (de->d_name[0] == '.' || strstr(de->d_name, ".tmp"))
            continue;
        snprintf(path, sizeof(path), OUTDIR "/%s", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(line, sizeof(line), f)) {
            n++;
            if (strncmp(line, MARK_VALUE, strlen(MARK_VALUE)) == 0) {
                (*copied)++;
                if (!first_copy[0])
                    snprintf(first_copy, fcsz, "%s", line);
            } else {
                (*clean)++;
            }
        }
        fclose(f);
    }
    closedir(d);
    return n;
}

/* CREATOR MODE: runs in its own forked process so its executive row has no
 * user name (a fresh registration) and it can be the subreaper. */
static int creator_main(int report_fd)
{
    int r[4] = { 0, 0, 0, 0 };     /* define st, creations ok, copied, clean */
    char first_copy[300] = "";

    (void)prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    r[0] = (int)def("LNM$PROCESS_TABLE", MARK_NAME, MARK_VALUE);

    struct vms_procinfo me;
    memset(&me, 0, sizeof(me));
    (void)vms_kif_getjpi_self(&me);

    for (int i = 0; i < ITERATIONS; i++) {
        uint32_t pid = 0;
        struct dsc$descriptor_s img = mkdsc(SUBJECT);
        uint32_t st = sys$creprc(&pid, &img, NULL, NULL, NULL, NULL, NULL,
                                 NULL, 0, 0, 0, PRC$M_DETACH);
        if (st & 1)
            r[1]++;
    }
    /* Wait (bounded) for every created subject's line; reap as we go -- the
     * orphaned subjects are this subreaper's children. */
    for (int waited = 0; waited < 30000; waited += 50) {
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if (count_results(&r[2], &r[3], first_copy, sizeof(first_copy)) >= r[1])
            break;
        poll(NULL, 0, 50);
    }
    (void)count_results(&r[2], &r[3], first_copy, sizeof(first_copy));
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    if (write(report_fd, r, sizeof(r)) != (ssize_t)sizeof(r))
        return 1;
    {
        char u[sizeof(me.username) + 1];
        memcpy(u, me.username, sizeof(me.username));
        u[sizeof(me.username)] = '\0';
        if (write(report_fd, u, sizeof(u)) != (ssize_t)sizeof(u))
            return 1;
    }
    if (write(report_fd, first_copy, sizeof(first_copy)) != (ssize_t)sizeof(first_copy))
        return 1;
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 1 && strcmp(basename(argv[0]), basename((char *)SUBJECT)) == 0)
        return subject_main();

    printf("=== test_syssvc_creprc_detached_lnm (a detached process starts with no copied process logical names, rd vms-fe7) ===\n");
    if (!executive_present()) {
        printf("=== test_syssvc_creprc_detached_lnm: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    char self[512];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) {
        printf("  FAIL: cannot resolve this test's own image\n");
        return 1;
    }
    self[sl] = '\0';
    unlink(SUBJECT);
    CHECK(symlink(self, SUBJECT) == 0, "staged the subject image (this binary, in subject mode)");
    {
        DIR *d = opendir(OUTDIR);           /* a clean slate: no earlier run's lines */
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                char p[300];
                if (de->d_name[0] == '.') continue;
                snprintf(p, sizeof(p), OUTDIR "/%s", de->d_name);
                unlink(p);
            }
            closedir(d);
        }
        mkdir(OUTDIR, 0777);
    }

    int pfd[2];
    if (pipe(pfd) < 0) { printf("  FAIL: pipe\n"); return 1; }
    pid_t c = fork();
    if (c == 0) {
        close(pfd[0]);
        _exit(creator_main(pfd[1]));
    }
    close(pfd[1]);
    int r[4] = { -1, -1, -1, -1 };
    char user[64] = "", first_copy[300] = "";
    int ok = read(pfd[0], r, sizeof(r)) == (ssize_t)sizeof(r);
    if (ok) {
        struct vms_procinfo dummy;
        ok = read(pfd[0], user, sizeof(dummy.username) + 1) > 0 &&
             read(pfd[0], first_copy, sizeof(first_copy)) == (ssize_t)sizeof(first_copy);
    }
    waitpid(c, NULL, 0);

    CHECK(ok, "the creator reported back");
    CHECK(r[0] == SS$_NORMAL || r[0] == SS$_SUPERSEDE,
          "the creator defined " MARK_NAME " in its own LNM$PROCESS");
    CHECK(user[0] == '\0',
          "the creator has no executive user name, so $CREPRC took the un-ticketed detached path (the boot's startup-DCL shape)");
    CHECK(r[1] == ITERATIONS, "every detached $CREPRC succeeded");
    CHECK(r[2] + r[3] == r[1], "every created subject reported what its LNM$PROCESS held");
    if (r[2] > 0)
        printf("  INFO: %d of %d subjects saw the creator's name; first: %s", r[2], r[1], first_copy);
    /* negctl: creprc-detached-intermediate-not-held */
    CHECK(r[2] == 0,
          "no detached process started with a copy of its creator's (or any) process logical names");

    printf("=== test_syssvc_creprc_detached_lnm: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
