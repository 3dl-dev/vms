/*
 * test_lib_cli.c - Unit tests for the CLI$ callable interface + compiled-CLD
 *                  support (vms-8c1).
 *
 * Exercises the REAL parse path end to end:
 *   1. cli$compile_cld() compiles the vendored MMK command table
 *      (tests/corpus/tier3-mmk/mmk_cld.cld) into an in-memory table.
 *   2. cli$dcl_parse() parses an MMK-style command against that table.
 *   3. cli$present() / cli$get_value() retrieve qualifiers, parameters,
 *      keyword sub-values and CLD defaults, using the exact idioms MMK uses.
 *   4. An inline CLD covers error paths (VALUE(REQUIRED), unknown qualifier).
 *
 * This is the acceptance gate for vms-8c1: it proves parse of a compiled CLD,
 * present of a qualifier, and get of a value all work with real MMK data.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "descrip.h"
#include "ssdef.h"
#include "climsgdef.h"
#include "stsdef.h"
#include "libclidef.h"
#include "clitable.h"

/* MMK_CLD_PATH is passed unquoted from CMake (to avoid backslash escapes in
 * compile_commands.json); stringize it here. */
#define CLI_STR2(x) #x
#define CLI_STR(x)  CLI_STR2(x)
#define MMK_CLD_FILE CLI_STR(MMK_CLD_PATH)

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

static struct dsc$descriptor_s sdesc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* Mirror MMK's cli_get_value() wrapper exactly. */
static uint32_t cli_get_value(const char *argname, char *arg, int argsize)
{
    struct dsc$descriptor_s argnamd = sdesc(argname);
    struct dsc$descriptor_s argd;
    uint16_t arglen = 0;
    argd.dsc$w_length  = (uint16_t)(argsize - 1);
    argd.dsc$b_dtype   = DSC$K_DTYPE_T;
    argd.dsc$b_class   = DSC$K_CLASS_S;
    argd.dsc$a_pointer = arg;
    uint32_t status = cli$present(&argnamd);
    if ($VMS_STATUS_SUCCESS(status)) {
        status = cli$get_value(&argnamd, &argd, &arglen);
        if ($VMS_STATUS_SUCCESS(status)) *(arg + arglen) = '\0';
    }
    return status;
}

static uint32_t cli_present(const char *name)
{
    struct dsc$descriptor_s d = sdesc(name);
    return cli$present(&d);
}

/* ------------------------------------------------------------------ */
/* Severity contract -- MMK relies on these exact success/warning bits. */
/* ------------------------------------------------------------------ */
static void test_severity(void)
{
    printf("Testing CLI$ status severity contract...\n");
    check($VMS_STATUS_SUCCESS(CLI$_PRESENT),   "CLI$_PRESENT is success");
    check($VMS_STATUS_SUCCESS(CLI$_DEFAULTED), "CLI$_DEFAULTED is success");
    check($VMS_STATUS_SUCCESS(CLI$_COMMA),     "CLI$_COMMA is success");
    check(!$VMS_STATUS_SUCCESS(CLI$_ABSENT),   "CLI$_ABSENT is NOT success");
    check(!$VMS_STATUS_SUCCESS(CLI$_NEGATED),  "CLI$_NEGATED is NOT success");
}

/* ------------------------------------------------------------------ */
/* Compile the real MMK CLD and drive it like MMK does.                */
/* ------------------------------------------------------------------ */
static char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (len_out) *len_out = rd;
    return buf;
}

static void test_mmk_cld(void)
{
    printf("Testing compile + parse of the real MMK CLD...\n");

    size_t len = 0;
    char *src = read_file(MMK_CLD_FILE, &len);
    check(src != NULL, "read mmk_cld.cld");
    if (!src) return;

    struct dsc$descriptor_s srcd;
    srcd.dsc$w_length  = (uint16_t)len; /* CLD fits comfortably in 16 bits */
    srcd.dsc$b_dtype   = DSC$K_DTYPE_T;
    srcd.dsc$b_class   = DSC$K_CLASS_S;
    srcd.dsc$a_pointer = src;

    struct cli_command_table *tab = NULL;
    uint32_t st = cli$compile_cld(&srcd, &tab);
    check($VMS_STATUS_SUCCESS(st) && tab != NULL, "cli$compile_cld succeeds");
    free(src);
    if (!tab) return;

    check(tab->nverbs == 1 && strcmp(tab->verbs[0].name, "MMK") == 0,
          "compiled table defines verb MMK");
    check(tab->verbs[0].nquals >= 20, "MMK verb has its qualifier set");
    check(tab->verbs[0].nparams >= 1, "MMK verb has a parameter");

    /* A realistic MMK command line. */
    const char *cmdline =
        "MMK /DESCRIPTION=BUILD.MMS /MACRO=(DEBUG=1,OPT) /VERIFY=ALL "
        "/NOACTION /EXTENDED_SYNTAX=(GNU_SYNTAX) FOO.EXE BAR.EXE";
    struct dsc$descriptor_s cmdd = sdesc(cmdline);
    st = cli$dcl_parse(&cmdd, tab, NULL, NULL);
    check($VMS_STATUS_SUCCESS(st), "cli$dcl_parse succeeds on MMK command");

    char tmp[256];

    /* Single-valued qualifier. */
    check(cli_present("DESCRIPTION") == CLI$_PRESENT, "/DESCRIPTION present");
    st = cli_get_value("DESCRIPTION", tmp, sizeof(tmp));
    check(st == SS$_NORMAL && strcmp(tmp, "BUILD.MMS") == 0,
          "DESCRIPTION value == BUILD.MMS");

    /* Abbreviated qualifier name resolves. */
    check(cli_present("DESC") == CLI$_PRESENT, "abbreviation /DESC resolves");

    /* Value LIST retrieved with the MMK CLI$_COMMA/SS$_NORMAL contract. */
    check(cli_present("MACRO") == CLI$_PRESENT, "/MACRO present");
    {
        char v1[64] = {0}, v2[64] = {0};
        uint32_t s1 = cli_get_value("MACRO", v1, sizeof(v1));
        uint32_t s2 = cli_get_value("MACRO", v2, sizeof(v2));
        check(s1 == CLI$_COMMA && strcmp(v1, "DEBUG=1") == 0,
              "MACRO value 1 == DEBUG=1 with CLI$_COMMA");
        check(s2 == SS$_NORMAL && strcmp(v2, "OPT") == 0,
              "MACRO value 2 == OPT with SS$_NORMAL (last)");
    }

    /* Negated qualifier -> CLI$_NEGATED (warning severity). */
    check(cli_present("ACTION") == CLI$_NEGATED, "/NOACTION -> CLI$_NEGATED");

    /* Absent qualifier. */
    check(cli_present("FORCE") == CLI$_ABSENT, "/FORCE absent -> CLI$_ABSENT");

    /* /VERIFY=ALL. */
    check(cli_present("VERIFY") == CLI$_PRESENT, "/VERIFY present");
    st = cli_get_value("VERIFY", tmp, sizeof(tmp));
    check($VMS_STATUS_SUCCESS(st) && strcmp(tmp, "ALL") == 0, "VERIFY value == ALL");

    /* Keyword sub-value present/absent notation (QUAL.KEYWORD). */
    check(cli_present("EXTENDED_SYNTAX") == CLI$_PRESENT, "/EXTENDED_SYNTAX present");
    check(cli_present("EXTENDED_SYNTAX.GNU_SYNTAX") == CLI$_PRESENT,
          "keyword EXTENDED_SYNTAX.GNU_SYNTAX present");
    check(cli_present("EXTENDED_SYNTAX.CASE_SENSITIVE") == CLI$_ABSENT,
          "keyword EXTENDED_SYNTAX.CASE_SENSITIVE absent");

    /* Defaulted qualifier: RULES_FILE not given, has VALUE(DEFAULT="MMS$RULES"). */
    check(cli_present("RULES_FILE") == CLI$_DEFAULTED,
          "/RULES_FILE defaulted -> CLI$_DEFAULTED");
    st = cli_get_value("RULES_FILE", tmp, sizeof(tmp));
    check($VMS_STATUS_SUCCESS(st) && strcmp(tmp, "MMS$RULES") == 0,
          "RULES_FILE default value == MMS$RULES");

    /* Positional LIST parameter (TARGET), MMK's exact do/while loop. */
    check(cli_present("TARGET") == CLI$_PRESENT, "TARGET parameter present");
    {
        char t1[64] = {0}, t2[64] = {0};
        uint32_t s1 = cli_get_value("TARGET", t1, sizeof(t1));
        uint32_t s2 = cli_get_value("TARGET", t2, sizeof(t2));
        check(s1 == CLI$_COMMA && strcmp(t1, "FOO.EXE") == 0,
              "TARGET 1 == FOO.EXE (CLI$_COMMA)");
        check(s2 == SS$_NORMAL && strcmp(t2, "BAR.EXE") == 0,
              "TARGET 2 == BAR.EXE (SS$_NORMAL, last)");
    }

    cli$free_cld(tab);
}

/* ------------------------------------------------------------------ */
/* Default-on qualifier (DEFAULT attribute) + default keyword.         */
/* ------------------------------------------------------------------ */
static void test_default_on(void)
{
    printf("Testing DEFAULT-attribute qualifier + default keyword...\n");
    size_t len = 0;
    char *src = read_file(MMK_CLD_FILE, &len);
    if (!src) { check(0, "read mmk_cld.cld (default-on)"); return; }
    struct dsc$descriptor_s srcd = sdesc("");
    srcd.dsc$w_length = (uint16_t)len; srcd.dsc$a_pointer = src;
    struct cli_command_table *tab = NULL;
    (void)cli$compile_cld(&srcd, &tab);
    free(src);
    if (!tab) { check(0, "compile for default-on"); return; }

    struct dsc$descriptor_s cmdd = sdesc("MMK PROG.EXE");
    uint32_t st = cli$dcl_parse(&cmdd, tab, NULL, NULL);
    check($VMS_STATUS_SUCCESS(st), "parse bare 'MMK PROG.EXE'");

    /* EXTENDED_SYNTAX has the DEFAULT attribute -> present though not typed. */
    check(cli_present("EXTENDED_SYNTAX") == CLI$_PRESENT,
          "EXTENDED_SYNTAX present by DEFAULT attribute");
    /* MMS_SYNTAX is the DEFAULT keyword of its type set. */
    check(cli_present("EXTENDED_SYNTAX.MMS_SYNTAX") == CLI$_PRESENT,
          "default keyword EXTENDED_SYNTAX.MMS_SYNTAX present");
    check(cli_present("VERIFY") == CLI$_ABSENT, "VERIFY absent when not typed");

    cli$free_cld(tab);
}

/* ------------------------------------------------------------------ */
/* Error paths on an inline CLD: VALUE(REQUIRED) and unknown qualifier.*/
/* ------------------------------------------------------------------ */
static void test_errors(void)
{
    printf("Testing CLD error paths...\n");
    const char *cld =
        "MODULE TEST_CLD\n"
        "DEFINE VERB TEST\n"
        "    PARAMETER P1, LABEL=INFILE, VALUE(REQUIRED)\n"
        "    QUALIFIER OUTPUT, VALUE(REQUIRED), NONNEGATABLE\n"
        "    QUALIFIER LOG, NEGATABLE\n";
    struct dsc$descriptor_s cldd = sdesc(cld);
    struct cli_command_table *tab = NULL;
    uint32_t st = cli$compile_cld(&cldd, &tab);
    check($VMS_STATUS_SUCCESS(st) && tab, "compile inline CLD");
    if (!tab) return;

    /* /OUTPUT with a value: OK. */
    struct dsc$descriptor_s ok = sdesc("TEST /OUTPUT=RESULT.TXT INPUT.DAT /LOG");
    st = cli$dcl_parse(&ok, tab, NULL, NULL);
    check($VMS_STATUS_SUCCESS(st), "parse with required value present");
    char tmp[128];
    st = cli_get_value("OUTPUT", tmp, sizeof(tmp));
    check($VMS_STATUS_SUCCESS(st) && strcmp(tmp, "RESULT.TXT") == 0,
          "OUTPUT value == RESULT.TXT");
    check(cli_present("LOG") == CLI$_PRESENT, "/LOG present");
    st = cli_get_value("INFILE", tmp, sizeof(tmp));
    check($VMS_STATUS_SUCCESS(st) && strcmp(tmp, "INPUT.DAT") == 0,
          "parameter INFILE == INPUT.DAT");

    /* /OUTPUT without its required value: parse error (no fake success). */
    struct dsc$descriptor_s bad = sdesc("TEST /OUTPUT INPUT.DAT");
    st = cli$dcl_parse(&bad, tab, NULL, NULL);
    check(!$VMS_STATUS_SUCCESS(st), "missing required value -> error status");

    /* Unknown qualifier -> CLI$_IVQUAL. */
    struct dsc$descriptor_s unk = sdesc("TEST /BOGUS INPUT.DAT");
    st = cli$dcl_parse(&unk, tab, NULL, NULL);
    check(st == CLI$_IVQUAL, "unknown qualifier -> CLI$_IVQUAL");

    cli$free_cld(tab);
}

/* ------------------------------------------------------------------ */
/* DCL$TABLES: SET HOST as RTPAD.EXE reads it (rd vms-0a7 S2).         */
/* ------------------------------------------------------------------ */
static void test_dcl_tables_set_host(void)
{
    printf("Testing DCL$TABLES SET HOST (RTPAD's CLI$GET_VALUE)...\n");
    const struct cli_command_table *t = cli$$dcl_tables();
    check(t != NULL, "DCL$TABLES compiles");
    if (!t) return;
    char tmp[128];
    struct dsc$descriptor_s c1 = sdesc("SET HOST VAXC /USERNAME=DNTEST");
    uint32_t st = cli$dcl_parse(&c1, t, NULL, NULL);
    check(st == SS$_NORMAL, "SET HOST VAXC /USERNAME=DNTEST parses");
    st = cli_get_value("NODE", tmp, sizeof tmp);
    check($VMS_STATUS_SUCCESS(st) && strcmp(tmp, "VAXC") == 0, "NODE == VAXC");
    st = cli_get_value("USERNAME", tmp, sizeof tmp);
    check($VMS_STATUS_SUCCESS(st) && strcmp(tmp, "DNTEST") == 0, "USERNAME == DNTEST");

    struct dsc$descriptor_s c2 = sdesc("SET HOST 1.965/USER=X");
    st = cli$dcl_parse(&c2, t, NULL, NULL);
    check(st == SS$_NORMAL, "an abbreviated /USER parses");
    st = cli_get_value("NODE", tmp, sizeof tmp);
    check($VMS_STATUS_SUCCESS(st) && strcmp(tmp, "1.965") == 0, "NODE == 1.965");

    struct dsc$descriptor_s c3 = sdesc("SET HOST VAXC");
    st = cli$dcl_parse(&c3, t, NULL, NULL);
    check(st == SS$_NORMAL && cli_present("USERNAME") == CLI$_ABSENT,
          "no /USERNAME -> CLI$_ABSENT");

    struct dsc$descriptor_s c4 = sdesc("SET HOST VAXC /BOGUS");
    check(cli$dcl_parse(&c4, t, NULL, NULL) == CLI$_IVQUAL,
          "a qualifier SET HOST does not take -> CLI$_IVQUAL");
    struct dsc$descriptor_s c5 = sdesc("SET HOST VAXC /USERNAME");
    check(!$VMS_STATUS_SUCCESS(cli$dcl_parse(&c5, t, NULL, NULL)),
          "/USERNAME without its value -> error");
}

/* ------------------------------------------------------------------ */
/* PLACEMENT=LOCAL / POSITIONAL qualifiers and a comma/plus LIST       */
/* parameter (rd vms-3cb1): the shape of the LINK command, CDU manual  */
/* PLACEMENT and the CLI$PRESENT status CLI$_LOCPRES / CLI$_LOCNEG.    */
/* ------------------------------------------------------------------ */
static void test_placement(void)
{
    printf("Testing PLACEMENT=LOCAL/POSITIONAL + list parameter...\n");
    const char *cld =
        "DEFINE VERB LNK\n"
        "    PARAMETER P1, LABEL=FILE, VALUE(REQUIRED, LIST)\n"
        "    QUALIFIER EXECUTABLE, NEGATABLE, VALUE\n"
        "    QUALIFIER SHAREABLE, NEGATABLE, PLACEMENT=POSITIONAL, VALUE\n"
        "    QUALIFIER LIBRARY, PLACEMENT=LOCAL\n"
        "    QUALIFIER INCLUDE, PLACEMENT=LOCAL, VALUE(REQUIRED, LIST)\n"
        "    QUALIFIER MAP, NEGATABLE, VALUE\n";
    struct dsc$descriptor_s srcd = sdesc(cld);
    struct cli_command_table *tab = NULL;
    uint32_t st = cli$compile_cld(&srcd, &tab);
    check($VMS_STATUS_SUCCESS(st) && tab, "compile placement CLD");
    if (!tab) return;

    struct dsc$descriptor_s cmdd = sdesc(
        "LNK/SHAREABLE=X.EXE/MAP A, B/LIBRARY/INCLUDE=(M1,M2)+C/SHAREABLE,D /NOSHAREABLE");
    st = cli$dcl_parse(&cmdd, tab, NULL, NULL);
    check($VMS_STATUS_SUCCESS(st), "parse LINK-shaped command");

    char v[64];
    /* before any file value: the command-level placement */
    check(cli_present("SHAREABLE") == CLI$_PRESENT, "/SHAREABLE after the verb: PRESENT");
    st = cli_get_value("SHAREABLE", v, sizeof v);
    check(st == SS$_NORMAL && strcmp(v, "X.EXE") == 0, "/SHAREABLE value X.EXE");
    check(cli_present("LIBRARY") == CLI$_ABSENT, "LOCAL /LIBRARY: ABSENT at command level");
    check(cli_present("MAP") == CLI$_PRESENT, "global /MAP PRESENT");

    st = cli_get_value("FILE", v, sizeof v);
    check(st == CLI$_COMMA && strcmp(v, "A") == 0, "FILE 1 == A, CLI$_COMMA");
    check(cli_present("LIBRARY") == CLI$_ABSENT, "A: no /LIBRARY");
    check(cli_present("SHAREABLE") == CLI$_PRESENT, "A: POSITIONAL falls back to command PRESENT");

    st = cli_get_value("FILE", v, sizeof v);
    check(st == CLI$_CONCAT && strcmp(v, "B") == 0, "FILE 2 == B, CLI$_CONCAT (followed by +)");
    check(cli_present("LIBRARY") == CLI$_LOCPRES, "B/LIBRARY: CLI$_LOCPRES");
    check(cli_present("INCLUDE") == CLI$_LOCPRES, "B/INCLUDE: CLI$_LOCPRES");
    {
        char m1[16] = {0}, m2[16] = {0};
        uint32_t s1 = cli_get_value("INCLUDE", m1, sizeof m1);
        uint32_t s2 = cli_get_value("INCLUDE", m2, sizeof m2);
        check(s1 == CLI$_COMMA && strcmp(m1, "M1") == 0 && s2 == SS$_NORMAL && strcmp(m2, "M2") == 0,
              "B/INCLUDE=(M1,M2) values");
    }

    st = cli_get_value("FILE", v, sizeof v);
    check(st == CLI$_COMMA && strcmp(v, "C") == 0, "FILE 3 == C");
    check(cli_present("SHAREABLE") == CLI$_LOCPRES, "C/SHAREABLE: CLI$_LOCPRES");
    check(cli_present("LIBRARY") == CLI$_ABSENT, "C: no /LIBRARY");

    st = cli_get_value("FILE", v, sizeof v);
    check(st == SS$_NORMAL && strcmp(v, "D") == 0, "FILE 4 == D (last)");
    check(cli_present("SHAREABLE") == CLI$_LOCNEG, "D /NOSHAREABLE: CLI$_LOCNEG");
    check(!$VMS_STATUS_SUCCESS(CLI$_LOCNEG) && $VMS_STATUS_SUCCESS(CLI$_LOCPRES),
          "LOCPRES success, LOCNEG warning severity");

    st = cli_get_value("FILE", v, sizeof v);
    check(st == CLI$_ABSENT, "FILE exhausted -> CLI$_ABSENT");

    /* A LOCAL qualifier with no parameter value before it is refused. */
    struct dsc$descriptor_s bad = sdesc("LNK/LIBRARY A");
    check(cli$dcl_parse(&bad, tab, NULL, NULL) == CLI$_IVQUAL, "LOCAL qualifier after the verb: IVQUAL");

    cli$free_cld(tab);
}

int main(void)
{
    printf("=== CLI$ callable interface tests (vms-8c1) ===\n");
    test_severity();
    test_mmk_cld();
    test_default_on();
    test_errors();
    test_dcl_tables_set_host();
    test_placement();

    if (failures == 0) {
        printf("\nAll CLI$ tests passed.\n");
        return 0;
    }
    printf("\n%d CLI$ test(s) FAILED.\n", failures);
    return 1;
}
