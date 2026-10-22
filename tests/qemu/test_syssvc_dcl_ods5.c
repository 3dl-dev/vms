/*
 * test_syssvc_dcl_ods5.c - DCL on an ODS-5 volume under the two file-name
 * parse styles (rd vms-0b0, epic vms-810), against a real /dev/vms.
 *
 * The volume is the real OpenVMS Alpha V8.4 ODS-5 disk the harness attaches as
 * VDA500: (tests/lab/captures/ods5-alpha-20261022/ods5t.dsk). Its [TEST]
 * c++config.h holds the record "#define CXXCONFIG 1" the real system wrote.
 * The expected DCL behaviour is what lab-Alpha V8.4 did (CONSOLE.txt, session
 * 1; CONSOLE-6.txt, session 6).
 *
 * A DCL login subprocess, fed one script, proves:
 *   1. F$GETJPI("","PARSE_STYLE_PERM") and _IMAGE read TRADITIONAL at first;
 *   2. under TRADITIONAL, TYPE ...c^+^+config.h is refused %DCL-W-PARMDEL
 *      (negative control: the same command the EXTENDED half runs);
 *   3. SET PROCESS/PARSE_STYLE=EXTENDED -> both items read EXTENDED;
 *   4. TYPE ...C^+^+CONFIG.H prints the real record (escape + case-blind);
 *   5. OPEN/WRITE of dcl^+^+0b0.h and Dcl0b0Mixed.Txt create them with their
 *      own names; DIRECTORY lists them so (escaped, case kept);
 *   6. DELETE removes both; DIRECTORY finds none;
 *   7. SET PROCESS/PARSE_STYLE=TRADITIONAL -> TRADITIONAL again;
 *   8. SET PROCESS/PARSE_STYLE=BOGUS is %DCL-W-IVKEYW naming BOGUS;
 *   9. =(EXT) is EXTENDED; a bare /PARSE_STYLE is TRADITIONAL.
 * No /dev/vms -> SKIP (77).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "starlet.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

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

/* The text between the line "<from>" and the line "<to>" (markers the script writes). */
static void between(const char *out, const char *from, const char *to, char *v, size_t vsz)
{
    v[0] = '\0';
    const char *a = strstr(out, from), *b;
    if (!a) return;
    a += strlen(from);
    b = strstr(a, to);
    if (!b) b = a + strlen(a);
    size_t n = (size_t)(b - a) < vsz - 1 ? (size_t)(b - a) : vsz - 1;
    memcpy(v, a, n);
    v[n] = '\0';
}

/* `text` appears in seg as a whole DIRECTORY entry: at a line start or after
 * a blank (DIRECTORY lists names in columns), and followed by a blank, a line
 * end or the end. Case-sensitive: the case is part of what is proved. */
static int has_entry(const char *seg, const char *text)
{
    size_t tl = strlen(text);
    for (const char *p = strstr(seg, text); p; p = strstr(p + 1, text)) {
        char before = p == seg ? '\n' : p[-1], after = p[tl];
        if ((before == '\n' || before == ' ') &&
            (after == '\0' || after == ' ' || after == '\n' || after == '\r'))
            return 1;
    }
    return 0;
}

static int count_of(const char *s, const char *text)
{
    int n = 0;
    for (const char *p = strstr(s, text); p; p = strstr(p + 1, text))
        n++;
    return n;
}

int main(void)
{
    static char out[16384];
    char seg[4096], b[32];
    uint32_t mj, mn;
    const char *unit = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_ods5: DCL under PARSE_STYLE on a real ODS-5 volume (rd vms-0b0) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_ods5: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }
    if (vms_kif_disk_resolve("VDA500:", b, sizeof b, &mj, &mn) == SS$_NORMAL) unit = "VDA500";
    else if (vms_kif_disk_resolve("DKA500:", b, sizeof b, &mj, &mn) == SS$_NORMAL) unit = "DKA500";
    CHECK(unit != NULL, "the ODS-5 fixture disk is attached (VDA500:/DKA500:)");
    if (!unit) { printf("=== test_syssvc_dcl_ods5: %d passed, %d failed ===\n", pass, fail); return 1; }
    snprintf(b, sizeof b, "%s:", unit);
    CHECK(vms_kif_acp_mount(b) & 1, "$MOUNT the ODS-5 volume");

    char script[4096];
    snprintf(script, sizeof script,
        "WRITE SYS$OUTPUT \"P0=\" + F$GETJPI(\"\",\"PARSE_STYLE_PERM\")\n"
        "WRITE SYS$OUTPUT \"I0=\" + F$GETJPI(\"\",\"PARSE_STYLE_IMAGE\")\n"
        "WRITE SYS$OUTPUT \"<T1>\"\n"
        "TYPE %1$s:[TEST]c^+^+config.h\n"
        "WRITE SYS$OUTPUT \"</T1>\"\n"
        "SET PROCESS/PARSE_STYLE=EXTENDED\n"
        "WRITE SYS$OUTPUT \"P1=\" + F$GETJPI(\"\",\"PARSE_STYLE_PERM\")\n"
        "WRITE SYS$OUTPUT \"I1=\" + F$GETJPI(\"\",\"PARSE_STYLE_IMAGE\")\n"
        "WRITE SYS$OUTPUT \"<T2>\"\n"
        "TYPE %1$s:[TEST]C^+^+CONFIG.H\n"
        "WRITE SYS$OUTPUT \"</T2>\"\n"
        "OPEN/WRITE F %1$s:[TEST]dcl^+^+0b0.h\n"
        "WRITE F \"#define DCL0B0 1\"\n"
        "CLOSE F\n"
        "OPEN/WRITE F %1$s:[TEST]Dcl0b0Mixed.Txt\n"
        "WRITE F \"mixed\"\n"
        "CLOSE F\n"
        "WRITE SYS$OUTPUT \"<D1>\"\n"
        "DIRECTORY %1$s:[TEST]*0b0*.*\n"
        "WRITE SYS$OUTPUT \"</D1>\"\n"
        "WRITE SYS$OUTPUT \"<T3>\"\n"
        "TYPE %1$s:[TEST]DCL^+^+0B0.H\n"
        "WRITE SYS$OUTPUT \"</T3>\"\n"
        "DELETE %1$s:[TEST]dcl^+^+0b0.h;*\n"
        "DELETE %1$s:[TEST]Dcl0b0Mixed.Txt;*\n"
        "WRITE SYS$OUTPUT \"<D2>\"\n"
        "DIRECTORY %1$s:[TEST]*0b0*.*\n"
        "WRITE SYS$OUTPUT \"</D2>\"\n"
        "SET PROCESS/PARSE_STYLE=TRADITIONAL\n"
        "WRITE SYS$OUTPUT \"P2=\" + F$GETJPI(\"\",\"PARSE_STYLE_PERM\")\n"
        "WRITE SYS$OUTPUT \"<K1>\"\n"
        "SET PROCESS/PARSE_STYLE=BOGUS\n"
        "WRITE SYS$OUTPUT \"</K1>\"\n"
        "SET PROCESS/PARSE_STYLE=(EXT)\n"
        "WRITE SYS$OUTPUT \"P3=\" + F$GETJPI(\"\",\"PARSE_STYLE_PERM\")\n"
        "SET PROCESS/PARSE_STYLE\n"
        "WRITE SYS$OUTPUT \"P4=\" + F$GETJPI(\"\",\"PARSE_STYLE_PERM\")\n",
        unit);
    CHECK(run_dcl(script, out, sizeof out) == 0, "DCL (a login subprocess, input from a pipe) ran");
    printf("  ---- DCL ----\n%s  ---- end ----\n", out);

    CHECK(strstr(out, "P0=TRADITIONAL") && strstr(out, "I0=TRADITIONAL"),
          "F$GETJPI PARSE_STYLE_PERM / _IMAGE read TRADITIONAL at first");
    /* DCL writes its messages to stderr, unbuffered, and the WRITE output to
     * stdout, so a message does not land between the markers around its
     * command; each is asserted over the whole transcript instead, where only
     * one command of the script can produce it. */
    between(out, "<T1>", "</T1>", seg, sizeof seg);
    /* negctl: dcl-caret-always-escape */
    CHECK(count_of(out, "%DCL-W-PARMDEL, invalid parameter delimiter - check use of special characters") == 1 &&
          !strstr(seg, "CXXCONFIG"),
          "TRADITIONAL: TYPE c^+^+config.h is refused %DCL-W-PARMDEL, as lab-Alpha V8.4 refuses it");
    /* negctl: dcl-parse-style-not-stored */
    CHECK(strstr(out, "P1=EXTENDED") && strstr(out, "I1=EXTENDED"),
          "SET PROCESS/PARSE_STYLE=EXTENDED: F$GETJPI PARSE_STYLE_PERM / _IMAGE read EXTENDED");
    between(out, "<T2>", "</T2>", seg, sizeof seg);
    CHECK(strstr(seg, "#define CXXCONFIG 1") != NULL,
          "EXTENDED: TYPE C^+^+CONFIG.H prints the record the real system wrote");
    between(out, "<D1>", "</D1>", seg, sizeof seg);
    CHECK(has_entry(seg, "dcl^+^+0b0.h;1"),
          "EXTENDED: OPEN/WRITE dcl^+^+0b0.h created it under its own name (DIRECTORY: dcl^+^+0b0.h;1)");
    CHECK(has_entry(seg, "Dcl0b0Mixed.Txt;1"),
          "EXTENDED: OPEN/WRITE Dcl0b0Mixed.Txt kept its case (DIRECTORY: Dcl0b0Mixed.Txt;1)");
    between(out, "<T3>", "</T3>", seg, sizeof seg);
    CHECK(strstr(seg, "#define DCL0B0 1") != NULL, "EXTENDED: TYPE DCL^+^+0B0.H finds it case-blind");
    between(out, "<D2>", "</D2>", seg, sizeof seg);
    CHECK(!strstr(seg, "0b0.h;") && !strstr(seg, "Mixed.Txt;") &&
          count_of(out, "%DIRECT-W-NOFILES") == 1,
          "DELETE removed both; DIRECTORY lists neither (%DIRECT-W-NOFILES)");
    CHECK(strstr(out, "P2=TRADITIONAL") != NULL, "SET PROCESS/PARSE_STYLE=TRADITIONAL: TRADITIONAL again");
    CHECK(count_of(out, "%DCL-W-IVKEYW, unrecognized keyword - check validity and spelling") == 1 &&
          strstr(out, " \\BOGUS\\") != NULL, "SET PROCESS/PARSE_STYLE=BOGUS is %DCL-W-IVKEYW ... \\BOGUS\\");
    CHECK(strstr(out, "P3=EXTENDED") != NULL,
          "SET PROCESS/PARSE_STYLE=(EXT): abbreviated and parenthesised, EXTENDED");
    CHECK(strstr(out, "P4=TRADITIONAL") != NULL,
          "a bare SET PROCESS/PARSE_STYLE is TRADITIONAL, as lab-Alpha V8.4 sets it");

    printf("=== test_syssvc_dcl_ods5: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
