/*
 * test_rms_parse_ods5.c - RMS $PARSE's name engine against the real OpenVMS
 * Alpha V8.4 F$PARSE (rd vms-b9c, epic vms-810).
 *
 * The oracle is the console of the lab sessions themselves, replayed line by
 * line: tests/lab/captures/ods5-alpha-20261022/CONSOLE-3.txt (session 3) and
 * CONSOLE-4.txt (session 4), predictions and verdicts in PREREGISTERED-3.md /
 * RESULTS.md there. Each probe line the real system printed --
 *     C2B [][SYS$SYSROOT:[SYSMGR]a^+b.t;]
 * -- names its input ("a+b.t" and "a^+b.t" for character 0x2B) and gives the
 * expanded string F$PARSE returned for each ("" = the parse failed). The test
 * parses the same input with the same default device and directory and the
 * same parse style, and compares the expanded string byte for byte.
 *
 * Covered: every printable ASCII character unescaped and ^-escaped (EXTENDED),
 * hex escapes, a type with an escape, four characters under TRADITIONAL, and
 * the session-3 probes (case kept / upper-cased, multi-dot names, NAME and TYPE
 * parts, unescaped '+', Latin-1 and UCS-2 escapes, spaces, 236/237-character
 * names). Negative control: the same EXTENDED inputs parsed TRADITIONAL must
 * NOT reproduce the lower-case EXTENDED results.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "rms/rms.h"
#include "rms_internal.h"
#include "rmsdef.h"

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: "); \
        printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Parse `spec` against default `dna` at style `ext`; the expanded string, or
 * "" when the parse fails (as F$PARSE returns ""). */
static const char *xparse(const char *spec, const char *dna, int ext, struct rms_pname *pn)
{
    static char out[300];
    struct rms_pname tmp;
    if (!pn)
        pn = &tmp;
    if (rms_name_parse_ex(spec, strlen(spec), dna, strlen(dna), pn, ext) != RMS$_NORMAL) {
        out[0] = '\0';
        return out;
    }
    memcpy(out, pn->esa, pn->esl);
    out[pn->esl] = '\0';
    return out;
}

/* the bracketed fields of a probe line "TAG [a][b]..." */
static int fields(const char *line, char f[][300], int max)
{
    int n = 0;
    const char *p = strchr(line, ' ');
    while (p && n < max) {
        const char *a = strchr(p, '[');
        if (!a) break;
        /* a field ends at "]" followed by "[" or end of line; the field itself
         * holds [dir] brackets, so match the LAST ']' before "][" / EOL */
        const char *e = strstr(a + 1, "][");
        const char *eol = a + 1 + strlen(a + 1);
        while (eol > a && (eol[-1] == '\n' || eol[-1] == '\r')) eol--;
        if (!e || e > eol) e = eol - 1;
        if (*e != ']') break;
        snprintf(f[n], 300, "%.*s", (int)(e - a - 1), a + 1);
        n++;
        p = e + 1;
        if (*p != '[') break;
    }
    return n;
}

static FILE *open_capture(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, "r");
}

static void session4(const char *dir)
{
    const char *dna = "SYS$SYSROOT:[SYSMGR]";
    char line[1024], f[4][300], in1[32], in2[32];
    int seen = 0;
    FILE *fp = open_capture(dir, "CONSOLE-4.txt");

    CHECK(fp != NULL, "open CONSOLE-4.txt");
    if (!fp) return;
    while (fgets(line, sizeof line, fp)) {
        unsigned c;
        int ext = line[0] == 'C', n;
        if ((line[0] == 'C' || line[0] == 'T') && isxdigit((unsigned char)line[1]) &&
            isxdigit((unsigned char)line[2]) && sscanf(line + 1, "%2x [", &c) == 1 &&
            line[3] == ' ' && line[4] == '[') {
            n = fields(line, f, 2);
            if (n != 2) continue;
            snprintf(in1, sizeof in1, "a%cb.t", (char)c);
            snprintf(in2, sizeof in2, "a^%cb.t", (char)c);
            /* "a:b.t" fails on VMS because there is no device A:, which F$PARSE
             * checks; the name engine is syntax only (the device is resolved
             * later), so it is the one probe not replayable here. */
            if (c != ':')
            CHECK(strcmp(xparse(in1, dna, ext, NULL), f[0]) == 0,
                  "%s '%s': got '%s', VMS '%s'", ext ? "EXT" : "TRAD", in1, xparse(in1, dna, ext, NULL), f[0]);
            CHECK(strcmp(xparse(in2, dna, ext, NULL), f[1]) == 0,
                  "%s '%s': got '%s', VMS '%s'", ext ? "EXT" : "TRAD", in2, xparse(in2, dna, ext, NULL), f[1]);
            seen++;
        } else if (strncmp(line, "CDQ [", 5) == 0 && fields(line, f, 2) == 2) {
            CHECK(strcmp(xparse("a\"b.t", dna, 1, NULL), f[0]) == 0, "EXT 'a\"b.t'");
            CHECK(strcmp(xparse("a^\"b.t", dna, 1, NULL), f[1]) == 0, "EXT 'a^\"b.t': got '%s', VMS '%s'",
                  xparse("a^\"b.t", dna, 1, NULL), f[1]);
            seen++;
        } else if (line[0] == 'H' && line[3] == ' ' && fields(line, f, 1) == 1) {
            snprintf(in1, sizeof in1, "a^%.2sb.t", line + 1);
            CHECK(strcmp(xparse(in1, dna, 1, NULL), f[0]) == 0, "EXT '%s': got '%s', VMS '%s'",
                  in1, xparse(in1, dna, 1, NULL), f[0]);
            seen++;
        } else if (strncmp(line, "C3 [", 4) == 0 && fields(line, f, 2) == 2) {
            CHECK(strcmp(xparse("ab.t^+u", dna, 1, NULL), f[0]) == 0, "EXT 'ab.t^+u'");
            CHECK(strcmp(xparse("ab.t+u", dna, 1, NULL), f[1]) == 0, "EXT 'ab.t+u'");
            seen++;
        }
    }
    fclose(fp);
    CHECK(seen == 43, "session 4: %d probe lines replayed (the console holds 43)", seen);
}

static void session3(const char *dir)
{
    const char *t = "LDA1:[TEST]", *t2 = "LDA2:[000000]";
    char line[1024], f[4][300], longn[300], longn2[300];
    struct rms_pname pn;
    int seen = 0;
    FILE *fp = open_capture(dir, "CONSOLE-3.txt");

    CHECK(fp != NULL, "open CONSOLE-3.txt");
    if (!fp) return;
    memset(longn, 'g', 232); memcpy(longn + 232, ".hpp", 5);
    memset(longn2, 'g', 233); memcpy(longn2 + 233, ".hpp", 5);
    while (fgets(line, sizeof line, fp)) {
        int n = fields(line, f, 4);
#define EQ(i, spec, dn, ext) CHECK(n > (i) && strcmp(xparse(spec, dn, ext, NULL), f[i]) == 0, \
            "%s %s: got '%s', VMS '%s'", line[0] ? "probe" : "", spec, xparse(spec, dn, ext, NULL), n > (i) ? f[i] : "?")
        if (strncmp(line, "R1 [", 4) == 0)        { EQ(0, "c^+^+config.h", t, 1); seen++; }
        else if (strncmp(line, "R3 [", 4) == 0)   { EQ(0, "a.b.c.d", t, 1); seen++; }
        else if (strncmp(line, "R5 [", 4) == 0)   {
            xparse("a.b.c.d", t, 1, &pn);
            CHECK(n == 2 && pn.name_len == strlen(f[0]) && memcmp(pn.esa + pn.name_off, f[0], pn.name_len) == 0 &&
                  pn.type_len == strlen(f[1]) && memcmp(pn.esa + pn.type_off, f[1], pn.type_len) == 0,
                  "R5 NAME/TYPE parts of a.b.c.d");
            seen++;
        }
        else if (strncmp(line, "R11 [", 5) == 0)  { EQ(0, "c++config.h", t, 1); seen++; }
        else if (strncmp(line, "RM [", 4) == 0)   { EQ(0, "MixedCase.Txt", t, 1); seen++; }
        else if (strncmp(line, "RL [", 4) == 0)   { EQ(1, "caf^E9.txt", t, 1); seen++; }
        else if (strncmp(line, "RU [", 4) == 0)   { EQ(1, "^u4e2d.TXT", t, 1); seen++; }
        else if (strncmp(line, "RS [", 4) == 0)   { EQ(1, "a^ b.txt", t, 1); EQ(2, "a b.txt", t, 1); seen++; }
        else if (strncmp(line, "R2 [", 4) == 0)   { EQ(0, "c^+^+config.h", t, 0); seen++; }
        else if (strncmp(line, "R4 [", 4) == 0)   { EQ(0, "a.b.c.d", t, 0); seen++; }
        else if (strncmp(line, "R2B [", 5) == 0)  { EQ(0, "MixedCase.Txt", t, 0); seen++; }
        else if (strncmp(line, "R9 [", 4) == 0)   {
            EQ(0, "c^+^+config.h", t2, 1); EQ(1, "a.b.c.d", t2, 1); EQ(2, "MixedCase.Txt", t2, 1); seen++;
        }
        else if (strncmp(line, "R8 [", 4) == 0)   {
            CHECK(n == 4 && (int)strlen(xparse(longn, t, 1, NULL)) == atoi(f[1]) &&
                  (int)strlen(xparse(longn2, t, 1, NULL)) == atoi(f[3]),
                  "R8: the 236- and 237-character names parse to the real lengths");
            seen++;
        }
#undef EQ
    }
    fclose(fp);
    CHECK(seen == 13, "session 3: %d probe lines replayed (expect 13)", seen);
}

static void negative_control(void)
{
    /* the EXTENDED results are not reproduced under TRADITIONAL (case) */
    CHECK(strcmp(xparse("c^+^+config.h", "LDA1:[TEST]", 0, NULL), "LDA1:[TEST]c^+^+config.h;") != 0,
          "TRADITIONAL does not keep the case EXTENDED keeps (negative control)");
    CHECK(strcmp(xparse("MixedCase.Txt", "LDA1:[TEST]", 1, NULL), "LDA1:[TEST]MixedCase.Txt;") == 0 &&
          strcmp(xparse("MixedCase.Txt", "LDA1:[TEST]", 0, NULL), "LDA1:[TEST]MIXEDCASE.TXT;") == 0,
          "the style alone decides the case");
    /* ODS-2-era specs are unchanged */
    CHECK(strcmp(xparse("SYS$SYSTEM:LOGINOUT.EXE", "", 0, NULL), "SYS$SYSTEM:LOGINOUT.EXE;") == 0,
          "a plain ODS-2 spec parses as before");
    CHECK(strcmp(xparse("dka0:[sys0.syscommon]a.b;3", "", 0, NULL), "DKA0:[SYS0.SYSCOMMON]A.B;3") == 0,
          "TRADITIONAL upper-cases device, directory, name, type");
}

int main(void)
{
    const char *dir = getenv("ODS5_FIXTURE_DIR");
    if (!dir) { printf("FAIL: ODS5_FIXTURE_DIR not set\n"); return 1; }
    session4(dir);
    session3(dir);
    negative_control();
    printf("test_rms_parse_ods5: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
