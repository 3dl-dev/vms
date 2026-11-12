/*
 * test_syssvc_crtl_stdio_start.c - a static product image reads its stdin from
 * SYS$INPUT from image start-up, through the C RTL device layer (rd vms-07ae).
 *
 * WHAT VMS DOES. A C image's stdin is the process's SYS$INPUT, opened by the
 * C RTL before main(): an image whose SYS$INPUT is a mailbox reads the
 * mailbox's messages, and end of file when the writer sends one (VSI C RTL
 * Reference, "Standard I/O"; System Services Reference, $CREMBX/IO$_WRITEOF).
 *
 * THE DECISIVE SHAPE. The subject is the shipped SYSGEN.EXE, a static utility
 * image whose main() reads commands with fgets(stdin) and calls no C RTL
 * install of its own. Its substrate descriptor 0 is /dev/null (no command at
 * all there); its SYS$INPUT is a mailbox holding "SHOW SCSNODE" then end of
 * file. Only an image whose start-up bound stdin to SYS$INPUT
 * (src/vmsrms/crtl_stdio_start.c, linked into every static product image by
 * CMakeLists.txt) can print the SCSNODE parameter; one reading descriptor 0
 * sees end of file at once and prints only its banner.
 *
 * Registered only where the start-up is linked (OVMX_CRTL_STDIO_START: the C
 * RTL base with the system-call funnel). NEGATIVE CONTROL:
 * tests/qemu/defects.d/crtl-static-stdio-not-bound.defect. No /dev/vms: SKIP.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "dvidef.h"
#include "iodef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define SYSGEN "/vms/SYS0/SYSCOMMON/SYSEXE/SYSGEN.EXE"

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
    fflush(stdout);
}

static int mbx_put(uint16_t chan, const char *s)
{
    uint16_t iosb[4] = { 0, 0, 0, 0 };
    uint32_t st = sys$qiow(0, chan, IO$_WRITEVBLK | IO$M_NOW, (void *)iosb, 0, 0,
                           (void *)s, strlen(s), 0, 0, 0, 0);
    return (st & 1) && (iosb[0] & 1);
}

int main(void)
{
    printf("=== test_syssvc_crtl_stdio_start: a static image's stdin is SYS$INPUT from start-up ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || vms_kif_open() < 0) {
        printf("=== test_syssvc_crtl_stdio_start: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    check(access(SYSGEN, X_OK) == 0, "SYSGEN.EXE is staged at SYS$SYSTEM");

    /* the mailbox SYS$INPUT: two records then end of file, all queued now */
    uint16_t chan = 0;
    check((sys$crembx(0, &chan, 512, 2048, 0, 0, NULL, 0) & 1) && chan,
          "$CREMBX made a temporary mailbox");
    char dev[64] = "";
    uint16_t dl = 0;
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = sizeof dev - 1;
    il[0].item_code = DVI$_DEVNAM;
    il[0].bufaddr = dev;
    il[0].retlen = &dl;
    check((sys$getdviw(0, chan, NULL, il, NULL, NULL, 0, 0) & 1) && dl, "$GETDVI names the mailbox");
    dev[dl] = '\0';
    uint16_t iosb[4] = { 0, 0, 0, 0 };
    check(mbx_put(chan, "SHOW SCSNODE\n") && mbx_put(chan, "EXIT\n") &&
          (sys$qiow(0, chan, IO$_WRITEOF | IO$M_NOW, (void *)iosb, 0, 0, 0, 0, 0, 0, 0, 0) & 1),
          "SHOW SCSNODE and EXIT, then end of file, are queued on the mailbox");

    int p[2];
    if (pipe(p) != 0) {
        check(0, "a pipe for the image's report");
        return 1;
    }
    fflush(stdout);
    pid_t pid = ovmx_fork_subprocess();
    if (pid == 0) {
        static const struct dsc$descriptor_s ptab =
            { 17, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$PROCESS_TABLE" };
        struct dsc$descriptor_s nd = { 9, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"SYS$INPUT" };
        struct item_list_3 li[2];
        memset(li, 0, sizeof li);
        li[0].buflen = (uint16_t)strlen(dev);
        li[0].item_code = LNM$_STRING;
        li[0].bufaddr = dev;
        if (!(sys$crelnm(NULL, &ptab, &nd, NULL, li) & 1))
            _exit(3);
        int nul = open("/dev/null", O_RDONLY);
        if (nul < 0 || dup2(nul, 0) != 0 || dup2(p[1], 1) != 1 || dup2(p[1], 2) != 2)
            _exit(4);
        close(p[0]);
        char *const av[] = { (char *)"SYSGEN.EXE", NULL };
        char *const ev[] = { (char *)"OVMX_SYSGEN_PATH=/tmp/ovmx07ae.par", NULL };
        execve(SYSGEN, av, ev);
        _exit(5);
    }
    close(p[1]);
    static char out[16384];
    size_t have = 0;
    ssize_t n;
    while (pid > 0 && have < sizeof out - 1 &&
           (n = read(p[0], out + have, sizeof out - 1 - have)) > 0)
        have += (size_t)n;
    out[have] = '\0';
    close(p[0]);
    int ws = 0;
    check(pid > 0 && waitpid(pid, &ws, 0) == pid && WIFEXITED(ws) && WEXITSTATUS(ws) < 3,
          "SYSGEN.EXE ran as a subprocess with SYS$INPUT the mailbox");
    printf("  --- SYSGEN.EXE wrote:\n%s  ---\n", out);
    check(strstr(out, "System Generation Utility") != NULL, "SYSGEN.EXE activated (its banner)");
    /* negctl: crtl-static-stdio-not-bound */
    check(strstr(out, "SCSNODE") != NULL,
          "SYSGEN.EXE read SHOW SCSNODE from the SYS$INPUT mailbox, not from descriptor 0");
    (void)sys$dassgn(chan);
    unlink("/tmp/ovmx07ae.par");
    printf("=== test_syssvc_crtl_stdio_start: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
