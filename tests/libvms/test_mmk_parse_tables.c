/*
 * test_mmk_parse_tables.c - drive the C port of MMK's parse_tables.mar
 * (bead vms-486) with representative description-file lines and object strings
 * and assert what the ported state machine actually did.
 *
 * The inputs are taken from the real vendored description file
 * tests/corpus/tier3-mmk/descrip.mms (directives, symbol defs, build rules,
 * dependencies, command lines) plus object-list forms PARSE_OBJECTS handles.
 * MMK upcases each line and sets TPA$M_BLANKS before calling lib$tparse, so we
 * do the same here.  No facade: every case runs the whole finite-state machine
 * through the real lib$table_parse engine and checks the fired PRS_K_ / PO_K_
 * codes and captured tokens.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "tpadef.h"
#include "lib$routines.h"
#include "mmk_parse_tables.h"

#ifndef $VMS_STATUS_SUCCESS
#define $VMS_STATUS_SUCCESS(code) ((code) & 1)
#endif

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

/* ---- run helpers (mirror parse_descrip / parse_objects call setup) ---- */
static uint32_t run_descrip(mmk_parse_ctx *m, const char *line) {
    memset(m, 0, sizeof *m);
    m->tpa.tpa$l_count     = TPA$K_COUNT0;
    m->tpa.tpa$l_options   = TPA$M_BLANKS;         /* MMK sets BLANKS */
    m->tpa.tpa$l_stringcnt = (uint32_t)strlen(line);
    m->tpa.tpa$l_stringptr = (char *)line;
    return lib$table_parse(m, &mmk_parse_descrip_grammar, NULL);
}
static uint32_t run_objects(mmk_parse_ctx *m, const char *line) {
    memset(m, 0, sizeof *m);
    m->tpa.tpa$l_count     = TPA$K_COUNT0;
    m->tpa.tpa$l_options   = TPA$M_BLANKS;
    m->tpa.tpa$l_stringcnt = (uint32_t)strlen(line);
    m->tpa.tpa$l_stringptr = (char *)line;
    return lib$table_parse(m, &mmk_parse_objects_grammar, NULL);
}

static int has_code(const mmk_parse_ctx *m, uint32_t c) {
    for (int i = 0; i < m->nevents; i++) if (m->code[i] == c) return 1;
    return 0;
}
static int count_code(const mmk_parse_ctx *m, uint32_t c) {
    int n = 0; for (int i = 0; i < m->nevents; i++) if (m->code[i] == c) n++;
    return n;
}
static const char *tok_for(const mmk_parse_ctx *m, uint32_t c) {
    for (int i = 0; i < m->nevents; i++) if (m->code[i] == c) return m->tok[i];
    return "";
}
static void dump(const char *label, const mmk_parse_ctx *m, uint32_t st) {
    printf("      [%s] status=%s codes=", label,
           $VMS_STATUS_SUCCESS(st) ? "OK" : "ERR");
    for (int i = 0; i < m->nevents; i++) printf("%u ", m->code[i]);
    printf("\n");
}

int main(void) {
    mmk_parse_ctx m;
    uint32_t st;

    printf("== MMK parse_tables.mar C port (bead vms-486) ==\n");

    /* ============================ PARSE_DESCRIP ============================ */

    /* --- conditional directives --- */
    st = run_descrip(&m, ".IFDEF ARCH");
    dump(".IFDEF ARCH", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && m.nevents == 1 &&
          m.code[0] == PRS_K_DIR_IFDEF && strcmp(tok_for(&m, PRS_K_DIR_IFDEF), "ARCH") == 0,
          ".IFDEF ARCH -> DIR_IFDEF, symbol=ARCH");

    st = run_descrip(&m, ".IFNDEF __VAX__");
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DIR_IFNDEF) &&
          strcmp(tok_for(&m, PRS_K_DIR_IFNDEF), "__VAX__") == 0,
          ".IFNDEF __VAX__ -> DIR_IFNDEF, symbol=__VAX__");

    st = run_descrip(&m, ".ELSE");
    CHECK($VMS_STATUS_SUCCESS(st) && m.nevents == 1 && m.code[0] == PRS_K_DIR_ELSE,
          ".ELSE -> DIR_ELSE");

    st = run_descrip(&m, ".ENDIF");
    CHECK($VMS_STATUS_SUCCESS(st) && m.nevents == 1 && m.code[0] == PRS_K_DIR_ENDIF,
          ".ENDIF -> DIR_ENDIF");

    /* --- .SUFFIXES and (crucially) the V2.8 .SUFFIXES_* variants: proves the
     *     keyword word-boundary — 'SUFFIXES' must NOT swallow 'SUFFIXES_AFTER' --- */
    st = run_descrip(&m, ".SUFFIXES : .PS .PDF");
    dump(".SUFFIXES", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DIR_SFX) &&
          !has_code(&m, PRS_K_DIR_SFX_AFTER) && has_code(&m, PRS_K_DIR_RHS),
          ".SUFFIXES : ... -> DIR_SFX + DIR_RHS");

    st = run_descrip(&m, ".SUFFIXES_AFTER : .OBJ");
    dump(".SUFFIXES_AFTER", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DIR_SFX_AFTER) &&
          !has_code(&m, PRS_K_DIR_SFX),
          ".SUFFIXES_AFTER -> DIR_SFX_AFTER, NOT DIR_SFX (word boundary)");

    st = run_descrip(&m, ".SUFFIXES_DELETE : .LIS");
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DIR_SFX_DELETE) &&
          !has_code(&m, PRS_K_DIR_SFX),
          ".SUFFIXES_DELETE -> DIR_SFX_DELETE, NOT DIR_SFX");

    st = run_descrip(&m, ".FIRST");
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DIR_FIRST),
          ".FIRST -> DIR_FIRST");

    st = run_descrip(&m, ".SILENT");
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DIR_SILENT),
          ".SILENT -> DIR_SILENT");

    /* --- symbol definitions (SET/append/etc) --- */
    st = run_descrip(&m, "ARCH = ALPHA");
    dump("ARCH = ALPHA", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_SYM_INIT) &&
          strcmp(tok_for(&m, PRS_K_SYM_INIT), "ARCH") == 0 &&
          has_code(&m, PRS_K_SYM_VALUE),
          "ARCH = ALPHA -> SYM_INIT(ARCH) + SYM_VALUE");

    st = run_descrip(&m, "OBJECTS = MMK.OBJ");
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_SYM_INIT) &&
          strcmp(tok_for(&m, PRS_K_SYM_INIT), "OBJECTS") == 0 &&
          has_code(&m, PRS_K_SYM_VALUE),
          "OBJECTS = MMK.OBJ -> SYM_INIT(OBJECTS) + SYM_VALUE");

    st = run_descrip(&m, "CFLAGS += /DEBUG");
    dump("CFLAGS += /DEBUG", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_SYM_INIT) &&
          has_code(&m, PRS_K_SYM_APPEND),
          "CFLAGS += /DEBUG -> SYM_INIT + SYM_APPEND");

    st = run_descrip(&m, "FOO ?= BAR");
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_SYM_DEFINED),
          "FOO ?= BAR -> SYM_DEFINED (conditional define)");

    /* --- build (inference) rule --- */
    st = run_descrip(&m, ".PS.PDF :");
    dump(".PS.PDF :", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_RULE_INIT) &&
          has_code(&m, PRS_K_RULE_SFX) && has_code(&m, PRS_K_RULE_END),
          ".PS.PDF : -> RULE_INIT + RULE_SFX + RULE_END");

    /* --- dependency: target that looks like a symbol reclassifies via SYM2DEP2 --- */
    st = run_descrip(&m, "MMK.OBJ : MMK.C");
    dump("MMK.OBJ : MMK.C", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_SYM_INIT) &&
          has_code(&m, PRS_K_SYM2DEP2) && has_code(&m, PRS_K_DEP_RHS),
          "MMK.OBJ : MMK.C -> SYM_INIT + SYM2DEP2 + DEP_RHS");

    /* --- dependency: non-symbol target drives the pure DEPEND path --- */
    st = run_descrip(&m, "1TARGET : DEP");
    dump("1TARGET : DEP", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DEP_INIT) &&
          has_code(&m, PRS_K_DEP_TRGAPP) && has_code(&m, PRS_K_DEP_RHS),
          "1TARGET : DEP -> DEP_INIT + DEP_TRGAPP + DEP_RHS");

    /* --- command lines: leading blank + prefix chars.  CMD_SETFLAGS is only
     *     stored when the CMD_PREFIXED subexpr SUCCEEDS, which requires the
     *     blank-pre-skip-restore engine fix; hence it proves that fix too. --- */
    st = run_descrip(&m, " @ WRITE SYS$OUTPUT X");
    dump("@ WRITE", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_CMD_INIT) &&
          has_code(&m, PRS_K_CMD_NOECHO) && has_code(&m, PRS_K_CMD_SETFLAGS) &&
          has_code(&m, PRS_K_CMD_TEXT),
          " @ WRITE ... -> CMD_INIT + CMD_NOECHO + CMD_SETFLAGS + CMD_TEXT");

    st = run_descrip(&m, " - PIPE GS FOO");
    dump("- PIPE", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_CMD_IGNERR) &&
          has_code(&m, PRS_K_CMD_TEXT),
          " - PIPE ... -> CMD_IGNERR + CMD_TEXT");

    st = run_descrip(&m, " CC FILEIO");
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_CMD_INIT) &&
          has_code(&m, PRS_K_CMD_TEXT) && !has_code(&m, PRS_K_CMD_NOECHO),
          " CC FILEIO -> CMD_INIT + CMD_TEXT (no prefix flags)");

    /* --- empty line (V2.3-1: allow empty description lines) --- */
    st = run_descrip(&m, "");
    CHECK($VMS_STATUS_SUCCESS(st), "empty line parses (EOS at COLUMN1)");

    /* --- GNU-syntax path only fires when gnu_mode is on --- */
    st = run_descrip(&m, "ENDIF");            /* no leading dot */
    (void)st;                                 /* bare ENDIF is a symbol/dep, not a directive */
    CHECK(!has_code(&m, PRS_K_DIR_ENDIF),
          "'ENDIF' (no dot, gnu off) is NOT a GNU .ENDIF");
    {
        memset(&m, 0, sizeof m);
        m.tpa.tpa$l_count = TPA$K_COUNT0;
        m.tpa.tpa$l_options = TPA$M_BLANKS;
        m.gnu_mode = 1;                        /* enable GNU syntax gate */
        const char *ln = "ENDIF";
        m.tpa.tpa$l_stringcnt = (uint32_t)strlen(ln);
        m.tpa.tpa$l_stringptr = (char *)ln;
        st = lib$table_parse(&m, &mmk_parse_descrip_grammar, NULL);
        dump("GNU ENDIF", &m, st);
        CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PRS_K_DIR_ENDIF),
              "'ENDIF' with gnu_mode -> GNU DIR_ENDIF (CHECK_GNU gate opens path)");
    }

    /* ============================ PARSE_OBJECTS =========================== */

    st = run_objects(&m, "FILEIO.OBJ");
    dump("FILEIO.OBJ", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PO_K_OBJ_INIT) &&
          has_code(&m, PO_K_APPNAM) && has_code(&m, PO_K_END_OBJ),
          "FILEIO.OBJ -> OBJ_INIT + APPNAM + END_OBJ");

    st = run_objects(&m, "A.OBJ,B.OBJ");
    dump("A.OBJ,B.OBJ", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && count_code(&m, PO_K_OBJ_INIT) == 2 &&
          count_code(&m, PO_K_END_OBJ) == 2,
          "A.OBJ,B.OBJ -> two objects (2x OBJ_INIT, 2x END_OBJ)");

    st = run_objects(&m, "MYLIB(MOD1)");
    dump("MYLIB(MOD1)", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PO_K_LIB_BEGIN) &&
          has_code(&m, PO_K_APPMOD) && has_code(&m, PO_K_LIB_END),
          "MYLIB(MOD1) -> LIB_BEGIN + APPMOD + LIB_END");

    st = run_objects(&m, "MYLIB(MOD1=MOD1.OBJ)");
    dump("MYLIB(MOD1=MOD1.OBJ)", &m, st);
    CHECK($VMS_STATUS_SUCCESS(st) && has_code(&m, PO_K_LIB_BEGIN) &&
          has_code(&m, PO_K_MOD_FILE) && has_code(&m, PO_K_APPFIL) &&
          has_code(&m, PO_K_LIB_END),
          "MYLIB(MOD1=MOD1.OBJ) -> LIB_BEGIN + MOD_FILE + APPFIL + LIB_END");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
