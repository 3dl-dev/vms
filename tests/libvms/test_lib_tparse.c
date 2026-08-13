/*
 * test_lib_tparse.c - unit tests for the lib$table_parse / lib$tparse TPARSE
 * engine (bead vms-9f6).
 *
 * These drive REAL grammars (built with the OVMX TPA_GRAMMAR table format) all
 * the way through lib$table_parse and assert on both the returned status AND on
 * the action-routine side effects (tokens captured, numbers parsed, param
 * dispatch, action-driven backtracking, subexpression matching).  No facade:
 * every case exercises the state machine.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "tpadef.h"
#include "lib$routines.h"

#ifndef $VMS_STATUS_SUCCESS
#define $VMS_STATUS_SUCCESS(code) ((code) & 1)
#endif

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

/* Extended TPARSE block: standard TPADEF first, then our own result fields. */
typedef struct {
    TPADEF   tpa;
    int      verb;
    char     name[64];
    uint32_t num;
    uint32_t uic;
    int      gate_fires, a_fires, b_fires;
    int      gate_allow;
    char     subtok[64];
} my_tpa;

static void copy_token(char *dst, size_t cap, TPADEF *t) {
    uint32_t n = t->tpa$l_tokencnt;
    if (n >= cap) n = (uint32_t)cap - 1;
    memcpy(dst, t->tpa$l_tokenptr, n);
    dst[n] = '\0';
}

/* ---- action routines (receive the block address, VMS convention) ---- */
static uint32_t act_verb(void *p)  { my_tpa *m = p; m->verb = (int)m->tpa.tpa$l_param; return SS$_NORMAL; }
static uint32_t act_name(void *p)  { my_tpa *m = p; copy_token(m->name, sizeof m->name, &m->tpa); return SS$_NORMAL; }
static uint32_t act_num(void *p)   { my_tpa *m = p; m->num = m->tpa.tpa$l_number; return SS$_NORMAL; }
static uint32_t act_uic(void *p)   { my_tpa *m = p; m->uic = m->tpa.tpa$l_number; return SS$_NORMAL; }
static uint32_t act_subtok(void *p){ my_tpa *m = p; copy_token(m->subtok, sizeof m->subtok, &m->tpa); return SS$_NORMAL; }
static uint32_t act_gate(void *p)  { my_tpa *m = p; m->gate_fires++; if (m->gate_allow) { m->a_fires++; return SS$_NORMAL; } return LIB$_SYNTAXERR; }
static uint32_t act_markb(void *p) { my_tpa *m = p; m->b_fires++; return SS$_NORMAL; }

static void init_block(my_tpa *m, const char *s, uint32_t options) {
    memset(m, 0, sizeof *m);
    m->tpa.tpa$l_count     = TPA$K_COUNT0;
    m->tpa.tpa$l_options   = options;
    m->tpa.tpa$l_stringcnt = (uint32_t)strlen(s);
    m->tpa.tpa$l_stringptr = (char *)s;
}

/* ================================================================
 * Grammar 1 — UIC parser:  [group,member] in octal, then end-of-string.
 * ================================================================ */
static const TPA_TRAN g1_s0[] = {
    { TPA$_UIC, NULL, 0, 1, act_uic, 0 },
};
static const TPA_TRAN g1_s1[] = {
    { TPA$_EOS, NULL, 0, TPA$_EXIT, NULL, 0 },
};
static const TPA_STATE g1_states[] = {
    { g1_s0, 1 },
    { g1_s1, 1 },
};
static const TPA_GRAMMAR g1 = { TPA$K_GRAMMAR_MAGIC, 2, g1_states };

/* ================================================================
 * Grammar 2 — a little command language:
 *   SET <symbol> = <decimal>
 *   SHOW <symbol>
 * ================================================================ */
enum { G2_START, G2_SET_NAME, G2_SET_EQ, G2_SET_VAL, G2_SHOW_NAME, G2_SHOW_END };
static const TPA_TRAN g2_start[] = {
    { TPA$K_KEYWORD, "SET",  0, G2_SET_NAME,  act_verb, 1 },
    { TPA$K_KEYWORD, "SHOW", 0, G2_SHOW_NAME, act_verb, 2 },
};
static const TPA_TRAN g2_set_name[]  = { { TPA$_SYMBOL,  NULL, 0, G2_SET_EQ,  act_name, 0 } };
static const TPA_TRAN g2_set_eq[]    = { { '=',          NULL, 0, G2_SET_VAL, NULL,     0 } };
static const TPA_TRAN g2_set_val[]   = { { TPA$_DECIMAL, NULL, 0, TPA$_EXIT,  act_num,  0 } };
static const TPA_TRAN g2_show_name[] = { { TPA$_SYMBOL,  NULL, 0, G2_SHOW_END,act_name, 0 } };
static const TPA_TRAN g2_show_end[]  = { { TPA$_EOS,     NULL, 0, TPA$_EXIT,  NULL,     0 } };
static const TPA_STATE g2_states[] = {
    { g2_start,     2 }, { g2_set_name, 1 }, { g2_set_eq,  1 },
    { g2_set_val,   1 }, { g2_show_name,1 }, { g2_show_end,1 },
};
static const TPA_GRAMMAR g2 = { TPA$K_GRAMMAR_MAGIC, 6, g2_states };

/* ================================================================
 * Grammar 3 — action-driven backtracking (MMK's CHECK_GNU pattern):
 * two lambda transitions; the first is gated by an action that may fail.
 * ================================================================ */
static const TPA_TRAN g3_start[] = {
    { TPA$_LAMBDA, NULL, 0, TPA$_EXIT, act_gate,  1 },
    { TPA$_LAMBDA, NULL, 0, TPA$_EXIT, act_markb, 2 },
};
static const TPA_STATE g3_states[] = { { g3_start, 2 } };
static const TPA_GRAMMAR g3 = { TPA$K_GRAMMAR_MAGIC, 1, g3_states };

/* ================================================================
 * Grammar 4 — subexpression: state 0 calls state 1 as a subexpression,
 * captures the whole matched span as the token.
 * ================================================================ */
static const TPA_TRAN g4_s0[] = {
    { TPA$K_SUBEXPR, NULL, 1, TPA$_EXIT, act_subtok, 0 },
};
static const TPA_TRAN g4_s1[] = {
    { TPA$K_KEYWORD, "AB", 0, TPA$_EXIT, NULL, 0 },
};
static const TPA_STATE g4_states[] = { { g4_s0, 1 }, { g4_s1, 1 } };
static const TPA_GRAMMAR g4 = { TPA$K_GRAMMAR_MAGIC, 2, g4_states };

/* ================================================================
 * Grammar 5 — keyword abbreviation (TPA$M_ABBREV).
 * ================================================================ */
static const TPA_TRAN g5_s0[] = {
    { TPA$K_KEYWORD, "INCLUDE", 0, TPA$_EXIT, NULL, 0 },
};
static const TPA_STATE g5_states[] = { { g5_s0, 1 } };
static const TPA_GRAMMAR g5 = { TPA$K_GRAMMAR_MAGIC, 1, g5_states };

/* ================================================================
 * Grammar 6 — keyword word-boundary (bead vms-486): a short keyword listed
 * BEFORE a longer keyword that shares its prefix must not swallow the longer
 * one.  This is exactly MMK's 'SUFFIXES' / 'SUFFIXES_AFTER' ordering.
 * ================================================================ */
static const TPA_TRAN g6_s0[] = {
    { TPA$K_KEYWORD, "SET",   0, TPA$_EXIT, act_verb, 1 },
    { TPA$K_KEYWORD, "SETUP", 0, TPA$_EXIT, act_verb, 2 },
};
static const TPA_STATE g6_states[] = { { g6_s0, 2 } };
static const TPA_GRAMMAR g6 = { TPA$K_GRAMMAR_MAGIC, 1, g6_states };

/* ================================================================
 * Grammar 7 — blank pre-skip must be undone on a failed match (bead vms-486):
 * MMK's CMD_PREFIXED pattern.  With TPA$M_BLANKS, the '@' transition pre-skips
 * the blank that the BLANK->EXIT terminator needs; a failed '@' match must
 * restore that blank so BLANK can still fire.
 * ================================================================ */
static const TPA_TRAN g7_s0[] = {
    { '@',       NULL, 0, 0,         act_markb, 0 },  /* loop on self (state 0) */
    { TPA$_BLANK,NULL, 0, TPA$_EXIT, NULL,      0 },
};
static const TPA_STATE g7_states[] = { { g7_s0, 2 } };
static const TPA_GRAMMAR g7 = { TPA$K_GRAMMAR_MAGIC, 1, g7_states };

int main(void) {
    my_tpa  m;
    uint32_t st;

    printf("== lib$table_parse / lib$tparse TPARSE engine ==\n");

    /* --- Grammar 1: UIC --- */
    init_block(&m, "[1,10]", 0);
    st = lib$table_parse(&m, &g1, NULL);
    CHECK($VMS_STATUS_SUCCESS(st), "UIC [1,10] parses");
    CHECK(m.uic == ((1u << 16) | 010u), "UIC [1,10] value = (1<<16)|010 (octal member)");

    init_block(&m, "[301,1]", 0);
    st = lib$table_parse(&m, &g1, NULL);
    CHECK($VMS_STATUS_SUCCESS(st), "UIC [301,1] parses");

    init_block(&m, "[9,10]", 0);   /* 9 is not octal -> reject (matches DEC demo) */
    st = lib$table_parse(&m, &g1, NULL);
    CHECK(st == LIB$_SYNTAXERR, "UIC [9,10] is a syntax error");

    init_block(&m, "1,10", 0);     /* missing brackets */
    st = lib$table_parse(&m, &g1, NULL);
    CHECK(st == LIB$_SYNTAXERR, "UIC 1,10 (no brackets) is a syntax error");

    /* --- Grammar 2: command language, with TPA$M_BLANKS --- */
    init_block(&m, "SET FOO = 42", TPA$M_BLANKS);
    st = lib$table_parse(&m, &g2, NULL);
    CHECK($VMS_STATUS_SUCCESS(st), "'SET FOO = 42' parses");
    CHECK(m.verb == 1,               "  verb dispatched via tpa$l_param (SET=1)");
    CHECK(strcmp(m.name, "FOO") == 0, "  symbol token captured = FOO");
    CHECK(m.num == 42,               "  decimal value captured = 42");

    init_block(&m, "SHOW BAR", TPA$M_BLANKS);
    st = lib$table_parse(&m, &g2, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && m.verb == 2 && strcmp(m.name, "BAR") == 0,
          "'SHOW BAR' parses, verb=2, name=BAR");

    init_block(&m, "SET FOO 42", TPA$M_BLANKS);   /* missing '=' */
    st = lib$table_parse(&m, &g2, NULL);
    CHECK(st == LIB$_SYNTAXERR, "'SET FOO 42' (missing =) is a syntax error");

    init_block(&m, "FROB", TPA$M_BLANKS);          /* unknown verb */
    st = lib$table_parse(&m, &g2, NULL);
    CHECK(st == LIB$_SYNTAXERR, "'FROB' (unknown verb) is a syntax error");

    /* --- Grammar 3: action-driven backtracking --- */
    init_block(&m, "", 0); m.gate_allow = 1;
    st = lib$table_parse(&m, &g3, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && m.a_fires == 1 && m.b_fires == 0,
          "gate open: first transition taken (a fired, b did not)");

    init_block(&m, "", 0); m.gate_allow = 0;
    st = lib$table_parse(&m, &g3, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && m.gate_fires == 1 && m.b_fires == 1,
          "gate closed: action rejects, engine backtracks to second transition");

    /* --- Grammar 4: subexpression --- */
    init_block(&m, "AB", 0);
    st = lib$table_parse(&m, &g4, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && strcmp(m.subtok, "AB") == 0,
          "subexpression matches 'AB' and outer token span = AB");

    init_block(&m, "XY", 0);
    st = lib$table_parse(&m, &g4, NULL);
    CHECK(st == LIB$_SYNTAXERR, "subexpression fails on 'XY' -> syntax error");

    /* --- Grammar 5: keyword abbreviation --- */
    init_block(&m, "INC", TPA$M_ABBREV);
    st = lib$table_parse(&m, &g5, NULL);
    CHECK($VMS_STATUS_SUCCESS(st), "abbrev on: 'INC' matches keyword INCLUDE");

    init_block(&m, "INC", 0);      /* abbrev off -> must be the full keyword */
    st = lib$table_parse(&m, &g5, NULL);
    CHECK(st == LIB$_SYNTAXERR, "abbrev off: 'INC' does not match INCLUDE");

    init_block(&m, "INCLUDE", 0);
    st = lib$table_parse(&m, &g5, NULL);
    CHECK($VMS_STATUS_SUCCESS(st), "full keyword 'INCLUDE' matches");

    /* --- Grammar 6: keyword word boundary --- */
    init_block(&m, "SET", 0);
    st = lib$table_parse(&m, &g6, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && m.verb == 1, "'SET' matches keyword SET (verb=1)");

    init_block(&m, "SETUP", 0);   /* must reach the LONGER keyword, not stop at SET */
    st = lib$table_parse(&m, &g6, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && m.verb == 2,
          "'SETUP' matches keyword SETUP not SET (word boundary; verb=2)");

    init_block(&m, "SETX", 0);    /* X is a symbol char -> neither keyword matches */
    st = lib$table_parse(&m, &g6, NULL);
    CHECK(st == LIB$_SYNTAXERR, "'SETX' matches neither SET nor SETUP (word boundary)");

    /* --- Grammar 7: blank pre-skip undone on failed match --- */
    init_block(&m, "@ ", TPA$M_BLANKS);
    st = lib$table_parse(&m, &g7, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && m.b_fires == 1,
          "'@ ' with BLANKS: '@' taken, then BLANK->EXIT still sees the blank");

    init_block(&m, "@@ ", TPA$M_BLANKS);
    st = lib$table_parse(&m, &g7, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && m.b_fires == 2,
          "'@@ ' with BLANKS: both '@' taken, BLANK terminates");

    /* --- lib$tparse alias resolves to the same engine --- */
    init_block(&m, "SHOW BAZ", TPA$M_BLANKS);
    st = lib$tparse(&m, &g2, NULL);
    CHECK($VMS_STATUS_SUCCESS(st) && strcmp(m.name, "BAZ") == 0,
          "lib$tparse alias drives the same engine");

    /* --- argument validation --- */
    init_block(&m, "AB", 0);
    st = lib$table_parse(NULL, &g4, NULL);
    CHECK(st == LIB$_INVARG, "NULL block -> LIB$_INVARG");
    st = lib$table_parse(&m, NULL, NULL);
    CHECK(st == LIB$_INVARG, "NULL grammar -> LIB$_INVARG");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
