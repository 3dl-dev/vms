/*
 * mmk_parse_tables.c - C port of MMK's parse_tables.mar (bead vms-486).
 *
 * A faithful, state-by-state transcription of the two TPARSE grammars in the
 * vendored MadGoat MMK freeware
 *     tests/corpus/tier3-mmk/parse_tables.mar  (V2.8, BSD-licensed)
 * into the OVMX TPA_GRAMMAR format (tpadef.h), driven by the real
 * lib$table_parse engine (src/libvms/rtl/lib_tparse.c, bead vms-9f6).
 *
 * WHY THIS EXISTS.  The .mar is a 704-line MACRO-32 module built from the
 * STARLET $STATE/$TRAN macros.  It has no C twin, and the OVMX self-host
 * contract forbids building a MACRO-32 assembler (and tcc's integrated
 * assembler is GAS-only).  The resolution for bead vms-486 is to express the
 * grammar directly in C against the engine that just landed — so MMK's
 * description-file / object-list parsing works with no MACRO-32 toolchain.
 *
 * CLEAN-ROOM (Rule 8).  Every state, transition, keyword and PRS_K_ / PO_K_
 * code here is transcribed from the freeware .mar's *structure and behaviour*.
 * No VSI/HPE/DEC source or binary was consulted.  The TPA_GRAMMAR in-memory
 * layout is an OVMX design choice (see tpadef.h); what matches VMS is the
 * observable parse, not the table bytes.
 *
 * TRANSLATION RULES (how a $STATE/$TRAN maps to a TPA_TRAN):
 *   - "$STATE label" and unlabeled "$STATE" become entries in the states[]
 *     array IN FILE ORDER.  Their index is the enum below.  State 0 is the
 *     $INIT_STATE's first state (the engine starts there).
 *   - $TRAN type,tostate[,action,,,code]:
 *       * type  -> a literal char (0..255), a TPA$_ class, TPA$K_KEYWORD for a
 *                  quoted keyword, or TPA$K_SUBEXPR for a "!label" call.
 *       * tostate omitted  -> TPA$K_NEXT_SEQ (fall through to the next state,
 *                  which is the VMS "next sequential $STATE" behaviour).
 *       * tostate TPA$_EXIT/TPA$_FAIL -> parse succeeds / fails.
 *       * action PRS_STORE/PO_STORE -> act_prs / act_po; the trailing PRS_K_ /
 *                  PO_K_* becomes the transition param (tpa$l_param), exactly as
 *                  the real PARSE_STORE / PARSE_OBJ_STORE dispatch on it.
 *   - "!label" (subexpression) -> TPA$K_SUBEXPR with target = that state.
 *
 * The MACRO literal aliases (LEFTPAREN=40 etc, .mar lines 186-196) are written
 * as the plain C characters '(' ')' '<' '>' '\'' ',' ';' '{' '|' '}' '~'.
 */

#include <stdint.h>
#include <string.h>

#include "ssdef.h"
#include "tpadef.h"
#include "lib$routines.h"
#include "mmk_parse_tables.h"

#ifndef $VMS_STATUS_SUCCESS
#define $VMS_STATUS_SUCCESS(code) ((code) & 1)
#endif

/* --------------------------------------------------------------------------
 * Action routines — the OVMX stand-ins for MMK's PARSE_STORE / PARSE_OBJ_STORE
 * (.mar declares them .EXTRN and calls them via the PRS_STORE/PO_STORE stubs,
 * lines 674-702).  Here they RECORD each fired code+token so a test can assert
 * what the ported state machine actually did — the state machine is real; only
 * the "store" side effect is a test probe.
 * -------------------------------------------------------------------------- */
#ifndef OVMX_MMK_PRODUCTION
static void mmk_record(mmk_parse_ctx *m, uint32_t code) {
    if (m->nevents >= MMK_MAX_EVENTS)
        return;
    uint32_t n = m->tpa.tpa$l_tokencnt;
    if (n >= MMK_TOKMAX)
        n = MMK_TOKMAX - 1;
    if (m->tpa.tpa$l_tokenptr != NULL && n > 0)
        memcpy(m->tok[m->nevents], m->tpa.tpa$l_tokenptr, n);
    m->tok[m->nevents][n] = '\0';
    m->code[m->nevents] = code;
    m->nevents++;
}
#endif /* !OVMX_MMK_PRODUCTION */

#ifdef OVMX_MMK_PRODUCTION
/* OVMX (vms-ec70): PRODUCTION build — this grammar is compiled INTO MMK.EXE and
 * its transitions must fire MMK's REAL store routines (parse_descrip.c's
 * parse_store / parse_objects.c's parse_obj_store), which take the TPARSE block
 * address and dispatch on tpa$l_param exactly like the .mar's PRS_STORE/PO_STORE.
 * The test-probe bodies below (which record into a mmk_parse_ctx) are compiled
 * out; the state machine itself is identical either way. */
extern int parse_store(void *tpablk);
extern int parse_obj_store(void *tpablk);
static uint32_t act_prs(void *blk) { return (uint32_t)parse_store(blk); }
static uint32_t act_po (void *blk) { return (uint32_t)parse_obj_store(blk); }
#else
static uint32_t act_prs(void *blk) {
    mmk_parse_ctx *m = (mmk_parse_ctx *)blk;
    uint32_t code = m->tpa.tpa$l_param;
    /* PARSE_STORE's two GATE transitions.  In real MMK these consult parser
     * state and may REJECT (return an even status) so the engine backtracks:
     *   - PRS_K_CHECK_GNU rejects unless GNU-syntax mode is on;
     *   - PRS_K_CHECK_COND rejects (skip the line) when inside a false .IF.
     * Modelling them faithfully lets a test drive both code paths; the defaults
     * (gnu_mode=0, cond_skip=0) are the common case. */
    if (code == PRS_K_CHECK_GNU && !m->gnu_mode)
        return LIB$_SYNTAXERR;
    if (code == PRS_K_CHECK_COND && m->cond_skip)
        return LIB$_SYNTAXERR;
    mmk_record(m, code);
    return SS$_NORMAL;
}

static uint32_t act_po(void *blk) {
    mmk_parse_ctx *m = (mmk_parse_ctx *)blk;
    mmk_record(m, m->tpa.tpa$l_param);
    return SS$_NORMAL;
}
#endif /* !OVMX_MMK_PRODUCTION */

/* --------------------------------------------------------------------------
 * State indices (enum == file order, so TPA$K_NEXT_SEQ = index+1 is faithful).
 * -------------------------------------------------------------------------- */
enum {                       /* PARSE_DESCRIP ($INIT_STATE PARSE_STATE) */
    PD_COLUMN1 = 0, PD_CHECK_COND, PD_CHECK_GNU, PD_GNU_DIR, PD_GNU_ELSE,
    PD_GNU_ELSIF, PD_GNU_IF0, PD_GNU_IF1, PD_GNU_IF1A, PD_GNU_IF2, PD_GNU_IF3,
    PD_GNU_IF4, PD_CONTINUE, PD_INCLUDE, PD_INC1, PD_DIRECTIVE, PD_DIRECTIVE_A,
    PD_DIR1, PD_DIR2, PD_IFDEF, PD_IFNDEF, PD_IF0, PD_IF0_A, PD_IF1, PD_IF1_A,
    PD_IF2, PD_IF2_A, PD_IF3, PD_IF4, PD_IF4_A, PD_BLDRULE0, PD_BLDRULE,
    PD_RULE1, PD_COMMAND, PD_CMD_PREFIXED, PD_SYMDEF, PD_SYMDEF0, PD_SYMDEF0A,
    PD_SYMDEF0B, PD_SYMDEF1, PD_SYMDEF2, PD_SYMDEF2A, PD_SYMDEF3, PD_SYMDEF3A,
    PD_SYMDEF4, PD_SYMDEF4A, PD_SYMDEF5, PD_SYMDEF5A, PD_DEPEND, PD_DEPEND0,
    PD_DEPEND1, PD_DEPEND1A, PD_DEPEND1B, PD_DEPEND2, PD_PATHPFX, PD_PATHPFX1,
    PD_STRSYM, PD_SYMBOL, PD_SYMBOL1, PD_SYMREF, PD_SYMREF0, PD_SYMREF1,
    PD_QUOTED, PD_DQUOTE, PD_DQUOTE1, PD_SQUOTE, PD_SQUOTE1, PD_NSTATES
};

enum {                       /* PARSE_OBJECTS ($INIT_STATE PO_STATE) */
    PO_INIT = 0, PO_INIT1, PO_INIT2, PO_INIT3, PO_CMSGEN1, PO_GEN, PO_GEN1,
    PO_GEN2, PO_GEN3, PO_GEN4, PO_GEN5, PO_GEN6, PO_GEN7, PO_GEN8, PO_GEN9,
    PO_CMSGEN2, PO_CMSGEN3, PO_CMSGEN4, PO_ENDOBJ, PO_LIB, PO_LIB1, PO_LIB2,
    PO_LIBFILE, PO_LIBFIL1, PO_LIBFIL2, PO_NSTATES
};

/* --------------------------------------------------------------------------
 * $TRAN shorthands.  TR/SUB carry no action; TRA/KWA/SUBA fire PARSE_STORE
 * (act_prs); OTR/OTRA fire PARSE_OBJ_STORE (act_po).
 * -------------------------------------------------------------------------- */
#define TR(type,nxt)          { (uint32_t)(type), NULL, 0, (uint32_t)(nxt), NULL,    0 }
#define TRA(type,nxt,param)   { (uint32_t)(type), NULL, 0, (uint32_t)(nxt), act_prs, (uint32_t)(param) }
#define KW(kw,nxt)            { TPA$K_KEYWORD, (kw), 0, (uint32_t)(nxt), NULL,    0 }
#define KWA(kw,nxt,param)     { TPA$K_KEYWORD, (kw), 0, (uint32_t)(nxt), act_prs, (uint32_t)(param) }
#define SUB(tgt,nxt)          { TPA$K_SUBEXPR, NULL, (uint32_t)(tgt), (uint32_t)(nxt), NULL,    0 }
#define SUBA(tgt,nxt,param)   { TPA$K_SUBEXPR, NULL, (uint32_t)(tgt), (uint32_t)(nxt), act_prs, (uint32_t)(param) }
#define OTR(type,nxt)         { (uint32_t)(type), NULL, 0, (uint32_t)(nxt), NULL,   0 }
#define OTRA(type,nxt,param)  { (uint32_t)(type), NULL, 0, (uint32_t)(nxt), act_po, (uint32_t)(param) }

/* ==========================================================================
 * PARSE_DESCRIP  (.mar lines 200-539)
 * ========================================================================== */

static const TPA_TRAN pd_column1[] = {          /* COLUMN1 (202) */
    TR (TPA$_EOS,    TPA$_EXIT),
    SUBA(PD_INCLUDE, TPA$_EXIT, PRS_K_DIR_INCLUDE),
    TR ('.',         PD_DIRECTIVE),
    SUB(PD_CHECK_GNU, PD_GNU_DIR),
    TR (TPA$_LAMBDA, PD_CHECK_COND),
};
static const TPA_TRAN pd_check_cond[] = {       /* CHECK_COND (209) */
    TRA(TPA$_LAMBDA, PD_CONTINUE, PRS_K_CHECK_COND),
};
static const TPA_TRAN pd_check_gnu[] = {        /* CHECK_GNU (212) */
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_CHECK_GNU),
};
static const TPA_TRAN pd_gnu_dir[] = {          /* GNU_DIR (215) */
    KWA("ENDIF", TPA$_EXIT, PRS_K_DIR_ENDIF),
    KW ("ELSE",  PD_GNU_ELSE),
    TR (TPA$_LAMBDA, PD_GNU_IF0),
};
static const TPA_TRAN pd_gnu_else[] = {         /* GNU_ELSE (220) */
    TR (TPA$_BLANK,  PD_GNU_ELSE),
    TRA(TPA$_EOS,    TPA$_EXIT,      PRS_K_DIR_ELSE),
    TRA(TPA$_LAMBDA, PD_GNU_ELSIF,   PRS_K_DIR_ELSIF),
};
static const TPA_TRAN pd_gnu_elsif[] = {        /* GNU_ELSIF (225) */
    TR (TPA$_BLANK,  PD_GNU_ELSIF),
    TR (TPA$_LAMBDA, PD_GNU_IF0),
};
static const TPA_TRAN pd_gnu_if0[] = {          /* GNU_IF0 (229) */
    KWA("IFEQ",  PD_GNU_IF1, PRS_K_DIR_IFEQL),
    KWA("IFNEQ", PD_GNU_IF1, PRS_K_DIR_IFNEQ),
    KW ("IFDEF",  PD_IFDEF),
    KW ("IFNDEF", PD_IFNDEF),
    TR (TPA$_LAMBDA, PD_CHECK_COND),
};
static const TPA_TRAN pd_gnu_if1[] = {          /* GNU_IF1 (236) */
    TR (TPA$_BLANK,  PD_GNU_IF1),
    TR ('(',         TPA$K_NEXT_SEQ),
    TR (TPA$_LAMBDA, TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_gnu_if1a[] = {         /* (anon, 241) */
    SUBA(PD_STRSYM, PD_GNU_IF2, PRS_K_DIR_IFLHS),
};
static const TPA_TRAN pd_gnu_if2[] = {          /* GNU_IF2 (244) */
    TR (TPA$_BLANK,  PD_GNU_IF2),
    TR (',',         PD_GNU_IF3),
    TR (TPA$_LAMBDA, PD_GNU_IF3),
};
static const TPA_TRAN pd_gnu_if3[] = {          /* GNU_IF3 (249) */
    TR (TPA$_BLANK, PD_GNU_IF3),
    SUBA(PD_STRSYM, PD_GNU_IF4, PRS_K_DIR_IFRHS),
};
static const TPA_TRAN pd_gnu_if4[] = {          /* GNU_IF4 (253) */
    TR (TPA$_BLANK, PD_GNU_IF4),
    TR (')',        TPA$_EXIT),
    TR (TPA$_EOS,   TPA$_EXIT),
};
static const TPA_TRAN pd_continue[] = {         /* CONTINUE (258) */
    TRA(TPA$_BLANK,  PD_COMMAND,  PRS_K_CMD_INIT),
    SUBA(PD_SYMBOL,  PD_SYMDEF,   PRS_K_SYM_INIT),
    SUBA(PD_PATHPFX, PD_BLDRULE0, PRS_K_RULE_INIPFX),
    TRA(TPA$_LAMBDA, PD_DEPEND,   PRS_K_DEP_INIT),
};
static const TPA_TRAN pd_include[] = {          /* INCLUDE (264) */
    KW ("INCLUDE", PD_INC1),
};
static const TPA_TRAN pd_inc1[] = {             /* INC1 (266) */
    TR (TPA$_BLANK,  PD_INC1),
    TR ('=',         TPA$_FAIL),
    TR (TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN pd_directive[] = {        /* DIRECTIVE (271) */
    KW ("IFDEF",  PD_IFDEF),
    KW ("IFNDEF", PD_IFNDEF),
    KWA("ELSE",   TPA$_EXIT, PRS_K_DIR_ELSE),
    KWA("ELSIF",  PD_IF0,    PRS_K_DIR_ELSIF),
    KWA("ENDIF",  TPA$_EXIT, PRS_K_DIR_ENDIF),
    KW ("IF",     PD_IF0),
    TRA(TPA$_LAMBDA, TPA$K_NEXT_SEQ, PRS_K_CHECK_COND),
};
static const TPA_TRAN pd_directive_a[] = {      /* (anon, 280) */
    KWA("SUFFIXES",        PD_DIR1,   PRS_K_DIR_SFX),
    KWA("SUFFIXES_AFTER",  PD_DIR1,   PRS_K_DIR_SFX_AFTER),
    KWA("SUFFIXES_BEFORE", PD_DIR1,   PRS_K_DIR_SFX_BEFORE),
    KWA("SUFFIXES_DELETE", PD_DIR1,   PRS_K_DIR_SFX_DELETE),
    KWA("FIRST",           PD_DIR1,   PRS_K_DIR_FIRST),
    KWA("LAST",            PD_DIR1,   PRS_K_DIR_LAST),
    KWA("BUILTIN",         TPA$_EXIT, PRS_K_DIR_BUILTIN),
    KWA("CASE_SENSITIVE",  TPA$_EXIT, PRS_K_DIR_CASE),
    KWA("GNU_SYNTAX",      TPA$_EXIT, PRS_K_DIR_GNU),
    KWA("SILENT",          TPA$_EXIT, PRS_K_DIR_SILENT),
    KWA("IGNORE",          TPA$_EXIT, PRS_K_DIR_IGNORE),
    KWA("DEFAULT",         TPA$_EXIT, PRS_K_DIR_DEFAULT),
    KWA("INCLUDE",         TPA$_EXIT, PRS_K_DIR_INCLUDE),
    TRA(TPA$_LAMBDA,       PD_BLDRULE, PRS_K_RULE_INIT),
};
static const TPA_TRAN pd_dir1[] = {             /* DIR1 (296) */
    TR (':',         PD_DIR2),
    TR (TPA$_BLANK,  PD_DIR1),
    TR (TPA$_EOS,    PD_DIR2),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_DIR_RHS),
};
static const TPA_TRAN pd_dir2[] = {             /* DIR2 (302) */
    TR (TPA$_BLANK,  PD_DIR2),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_DIR_RHS),
};
static const TPA_TRAN pd_ifdef[] = {            /* IFDEF (306) */
    TR (TPA$_BLANK, PD_IFDEF),
    SUBA(PD_SYMBOL, TPA$_EXIT, PRS_K_DIR_IFDEF),
};
static const TPA_TRAN pd_ifndef[] = {           /* IFNDEF (310) */
    TR (TPA$_BLANK, PD_IFNDEF),
    SUBA(PD_SYMBOL, TPA$_EXIT, PRS_K_DIR_IFNDEF),
};
static const TPA_TRAN pd_if0[] = {              /* IF0 (314) */
    TR (TPA$_BLANK,  PD_IF0),
    TR ('.',         TPA$K_NEXT_SEQ),
    TR (TPA$_LAMBDA, PD_IF1),
};
static const TPA_TRAN pd_if0_a[] = {            /* (anon, 319) */
    KWA("NOT", PD_IF1, PRS_K_DIR_NOT),
};
static const TPA_TRAN pd_if1[] = {              /* IF1 (322) */
    TR (TPA$_BLANK, PD_IF1),
    SUBA(PD_STRSYM, TPA$K_NEXT_SEQ, PRS_K_DIR_IFLHS),
};
static const TPA_TRAN pd_if1_a[] = {            /* (anon, 326) */
    SUB(PD_IF2, PD_IF4),
    TRA(TPA$_LAMBDA, PD_IF4, PRS_K_DIR_IFRHS),
};
static const TPA_TRAN pd_if2[] = {              /* IF2 (330) */
    TR (TPA$_BLANK, PD_IF2),
    TR ('.',        TPA$K_NEXT_SEQ),
    KWA("EQL", PD_IF3, PRS_K_DIR_IFEQL),
    KWA("NEQ", PD_IF3, PRS_K_DIR_IFNEQ),
    KWA("GEQ", PD_IF3, PRS_K_DIR_IFGEQ),
    KWA("LEQ", PD_IF3, PRS_K_DIR_IFLEQ),
    KWA("GTR", PD_IF3, PRS_K_DIR_IFGTR),
    KWA("LSS", PD_IF3, PRS_K_DIR_IFLSS),
};
static const TPA_TRAN pd_if2_a[] = {            /* (anon, 340) */
    KWA("EQ", PD_IF3, PRS_K_DIR_IFEQL),
    KWA("NE", PD_IF3, PRS_K_DIR_IFNEQ),
    KWA("GE", PD_IF3, PRS_K_DIR_IFGEQ),
    KWA("LE", PD_IF3, PRS_K_DIR_IFLEQ),
    KWA("GT", PD_IF3, PRS_K_DIR_IFGTR),
    KWA("LT", PD_IF3, PRS_K_DIR_IFLSS),
};
static const TPA_TRAN pd_if3[] = {              /* IF3 (348) */
    TR (TPA$_BLANK, PD_IF3),
    SUBA(PD_STRSYM, PD_IF4, PRS_K_DIR_IFRHS),
};
static const TPA_TRAN pd_if4[] = {              /* IF4 (352) */
    TR (TPA$_BLANK, PD_IF4),
    TR ('.',        TPA$K_NEXT_SEQ),
    TR (TPA$_EOS,   TPA$_EXIT),
};
static const TPA_TRAN pd_if4_a[] = {            /* (anon, 357) */
    KWA("AND", PD_IF0, PRS_K_DIR_AND),
    KWA("OR",  PD_IF0, PRS_K_DIR_OR),
};
static const TPA_TRAN pd_bldrule0[] = {         /* BLDRULE0 (361) */
    TR ('.', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_bldrule[] = {          /* BLDRULE (363) */
    TRA('.',         PD_BLDRULE, PRS_K_RULE_NEWSFX),
    TRA(':',         TPA$_EXIT,  PRS_K_RULE_END),
    TRA('$',         PD_BLDRULE, PRS_K_RULE_SFX),
    TRA('_',         PD_BLDRULE, PRS_K_RULE_SFX),
    TRA('-',         PD_BLDRULE, PRS_K_RULE_SFX),
    TRA('~',         PD_BLDRULE, PRS_K_RULE_SFX),
    TRA(TPA$_EOS,    TPA$_EXIT,  PRS_K_RULE_END),
    TR (TPA$_BLANK,  PD_RULE1),
    TRA(TPA$_ALPHA,  PD_BLDRULE, PRS_K_RULE_SFX),
    TRA(TPA$_DIGIT,  PD_BLDRULE, PRS_K_RULE_SFX),
    SUBA(PD_PATHPFX, PD_BLDRULE, PRS_K_RULE_NEWPFX),
};
static const TPA_TRAN pd_rule1[] = {            /* RULE1 (376) */
    TRA(':',        TPA$_EXIT, PRS_K_RULE_END),
    TRA(TPA$_EOS,   TPA$_EXIT, PRS_K_RULE_END),
    TR (TPA$_BLANK, PD_RULE1),
};
static const TPA_TRAN pd_command[] = {          /* COMMAND (381) */
    SUBA(PD_CMD_PREFIXED, PD_COMMAND, PRS_K_CMD_SETFLAGS),
    TR (TPA$_BLANK,  PD_COMMAND),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_CMD_TEXT),
};
static const TPA_TRAN pd_cmd_prefixed[] = {     /* CMD_PREFIXED (386) */
    TRA('@',        PD_CMD_PREFIXED, PRS_K_CMD_NOECHO),
    TRA('-',        PD_CMD_PREFIXED, PRS_K_CMD_IGNERR),
    TRA('<',        PD_CMD_PREFIXED, PRS_K_CMD_FFORCED),
    TRA('>',        PD_CMD_PREFIXED, PRS_K_CMD_LFORCED),
    TR (TPA$_BLANK, TPA$_EXIT),
};
static const TPA_TRAN pd_symdef[] = {           /* SYMDEF (393) */
    TR (TPA$_BLANK, PD_SYMDEF0),
    TR ('=',        PD_SYMDEF1),
    TR ('?',        PD_SYMDEF2),
    TR ('+',        PD_SYMDEF3),
    TR ('|',        PD_SYMDEF4),
    TR ('~',        PD_SYMDEF5),
    TRA(',',        PD_DEPEND, PRS_K_SYM2DEP),
    TRA('-',        PD_DEPEND, PRS_K_SYM2DEP),
    TRA('(',        PD_DEPEND, PRS_K_SYM2DEP),
    TRA(':',        PD_DEPEND, PRS_K_SYM2DEP),
    TRA('[',        PD_DEPEND, PRS_K_SYM2DEP),
    TRA(';',        PD_DEPEND, PRS_K_SYM2DEP),
};
static const TPA_TRAN pd_symdef0[] = {          /* SYMDEF0 (407) */
    TR (TPA$_BLANK,  PD_SYMDEF0),
    TR (TPA$_LAMBDA, TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_symdef0a[] = {         /* SYMDEF0A (411) */
    KW ("ADDITIONALLY_DEPENDS_ON", PD_SYMDEF0B),
    KWA("DEPENDS_ON", PD_DEPEND2, PRS_K_SYM2DEP2),
    TR ('=',         PD_SYMDEF1),
    TR ('?',         PD_SYMDEF2),
    TR ('+',         PD_SYMDEF3),
    TR ('|',         PD_SYMDEF4),
    TR ('~',         PD_SYMDEF5),
    TRA(',',         PD_DEPEND,   PRS_K_SYM2DEP),
    TRA(':',         PD_DEPEND1B, PRS_K_SYM2DEP2),
    TRA(TPA$_LAMBDA, PD_DEPEND1,  PRS_K_SYM2DEP2),
};
static const TPA_TRAN pd_symdef0b[] = {         /* SYMDEF0B (423) */
    TRA(TPA$_LAMBDA, PD_DEPEND2, PRS_K_DEP_DC),
};
static const TPA_TRAN pd_symdef1[] = {          /* SYMDEF1 (426) */
    TR (TPA$_BLANK,  PD_SYMDEF1),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_SYM_VALUE),
};
static const TPA_TRAN pd_symdef2[] = {          /* SYMDEF2 (430) */
    TR ('=', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_symdef2a[] = {         /* SYMDEF2A (432) */
    TR (TPA$_BLANK,  PD_SYMDEF2A),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_SYM_DEFINED),
};
static const TPA_TRAN pd_symdef3[] = {          /* SYMDEF3 (436) */
    TR ('=', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_symdef3a[] = {         /* SYMDEF3A (438) */
    TR (TPA$_BLANK,  PD_SYMDEF3A),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_SYM_APPEND),
};
static const TPA_TRAN pd_symdef4[] = {          /* SYMDEF4 (442) */
    TR ('=', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_symdef4a[] = {         /* SYMDEF4A (444) */
    TR (TPA$_BLANK,  PD_SYMDEF4A),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_SYM_DO),
};
static const TPA_TRAN pd_symdef5[] = {          /* SYMDEF5 (448) */
    TR ('=', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_symdef5a[] = {         /* SYMDEF5A (450) */
    TR (TPA$_BLANK,  PD_SYMDEF5A),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_SYM_EVAL),
};
static const TPA_TRAN pd_depend[] = {           /* DEPEND (454) */
    TR (TPA$_BLANK, PD_DEPEND1),
    TR (TPA$_EOS,   TPA$_FAIL),
    TRA(',',        PD_DEPEND0, PRS_K_DEP_TRGAPP),
    TRA(TPA$_ANY,   PD_DEPEND,  PRS_K_DEP_TRGAPP),
};
static const TPA_TRAN pd_depend0[] = {          /* DEPEND0 (460) */
    TR (':',         TPA$_FAIL),
    TR (',',         TPA$_FAIL),
    TR (TPA$_EOS,    TPA$_FAIL),
    TR (TPA$_BLANK,  PD_DEPEND0),
    TR (TPA$_LAMBDA, PD_DEPEND),
};
static const TPA_TRAN pd_depend1[] = {          /* DEPEND1 (467) */
    TR (TPA$_BLANK,  PD_DEPEND1),
    TR (TPA$_LAMBDA, TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_depend1a[] = {         /* DEPEND1A (471) */
    KWA("ADDITIONALLY_DEPENDS_ON", PD_DEPEND2, PRS_K_DEP_DC),
    KW ("DEPENDS_ON", PD_DEPEND2),
    TRA(',',        PD_DEPEND0, PRS_K_DEP_TRGAPP),
    TR (':',        PD_DEPEND1B),
    TR (TPA$_EOS,   TPA$_FAIL),
    TRA(TPA$_ANY,   PD_DEPEND,  PRS_K_DEP_TRGAPP2),
};
static const TPA_TRAN pd_depend1b[] = {         /* DEPEND1B (479) */
    TRA(':',         TPA$K_NEXT_SEQ, PRS_K_DEP_DC),
    TR (TPA$_LAMBDA, TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_depend2[] = {          /* DEPEND2 (483) */
    TR (TPA$_BLANK,  PD_DEPEND2),
    TRA(TPA$_LAMBDA, TPA$_EXIT, PRS_K_DEP_RHS),
};
static const TPA_TRAN pd_pathpfx[] = {          /* PATHPFX (487) */
    TR ('{', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_pathpfx1[] = {         /* PATHPFX1 (489) */
    TR ('}',      TPA$_EXIT),
    TR (TPA$_ANY, PD_PATHPFX1),
};
static const TPA_TRAN pd_strsym[] = {           /* STRSYM (493) */
    TR (TPA$_BLANK, PD_STRSYM),
    SUB(PD_QUOTED,  TPA$_EXIT),
    SUB(PD_SYMBOL,  TPA$_EXIT),
    TR (TPA$_STRING, TPA$_EXIT),
};
static const TPA_TRAN pd_symbol[] = {           /* SYMBOL (499) */
    SUB(PD_SYMREF, TPA$_EXIT),
    TR ('$',        PD_SYMBOL1),
    TR ('_',        PD_SYMBOL1),
    TR (TPA$_ALPHA, PD_SYMBOL1),
};
static const TPA_TRAN pd_symbol1[] = {          /* SYMBOL1 (504) */
    TR ('$',         PD_SYMBOL1),
    TR ('_',         PD_SYMBOL1),
    TR ('.',         PD_SYMBOL1),
    TR (TPA$_ALPHA,  PD_SYMBOL1),
    TR (TPA$_DIGIT,  PD_SYMBOL1),
    TR (TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN pd_symref[] = {           /* SYMREF (512) */
    TR ('$', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_symref0[] = {          /* SYMREF0 (514) */
    TR ('(', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN pd_symref1[] = {          /* SYMREF1 (516) */
    TR (')',        TPA$_EXIT),
    SUB(PD_SYMREF,  PD_SYMREF1),
    TR (TPA$_ANY,   PD_SYMREF1),
};
static const TPA_TRAN pd_quoted[] = {           /* QUOTED (521) */
    TR ('"',  PD_DQUOTE),
    TR ('\'', PD_SQUOTE),
};
static const TPA_TRAN pd_dquote[] = {           /* DQUOTE (525) */
    TR ('"',      TPA$K_NEXT_SEQ),
    TR (TPA$_ANY, PD_DQUOTE),
};
static const TPA_TRAN pd_dquote1[] = {          /* DQUOTE1 (528) */
    TR ('"',         PD_DQUOTE),
    TR (TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN pd_squote[] = {           /* SQUOTE (532) */
    TR ('\'',     TPA$K_NEXT_SEQ),
    TR (TPA$_ANY, PD_SQUOTE),
};
static const TPA_TRAN pd_squote1[] = {          /* SQUOTE1 (535) */
    TR ('\'',        PD_SQUOTE),
    TR (TPA$_LAMBDA, TPA$_EXIT),
};

#define ST(arr) { (arr), (uint32_t)(sizeof(arr) / sizeof((arr)[0])) }

static const TPA_STATE pd_states[PD_NSTATES] = {
    [PD_COLUMN1]     = ST(pd_column1),   [PD_CHECK_COND]  = ST(pd_check_cond),
    [PD_CHECK_GNU]   = ST(pd_check_gnu), [PD_GNU_DIR]     = ST(pd_gnu_dir),
    [PD_GNU_ELSE]    = ST(pd_gnu_else),  [PD_GNU_ELSIF]   = ST(pd_gnu_elsif),
    [PD_GNU_IF0]     = ST(pd_gnu_if0),   [PD_GNU_IF1]     = ST(pd_gnu_if1),
    [PD_GNU_IF1A]    = ST(pd_gnu_if1a),  [PD_GNU_IF2]     = ST(pd_gnu_if2),
    [PD_GNU_IF3]     = ST(pd_gnu_if3),   [PD_GNU_IF4]     = ST(pd_gnu_if4),
    [PD_CONTINUE]    = ST(pd_continue),  [PD_INCLUDE]     = ST(pd_include),
    [PD_INC1]        = ST(pd_inc1),      [PD_DIRECTIVE]   = ST(pd_directive),
    [PD_DIRECTIVE_A] = ST(pd_directive_a),[PD_DIR1]       = ST(pd_dir1),
    [PD_DIR2]        = ST(pd_dir2),      [PD_IFDEF]       = ST(pd_ifdef),
    [PD_IFNDEF]      = ST(pd_ifndef),    [PD_IF0]         = ST(pd_if0),
    [PD_IF0_A]       = ST(pd_if0_a),     [PD_IF1]         = ST(pd_if1),
    [PD_IF1_A]       = ST(pd_if1_a),     [PD_IF2]         = ST(pd_if2),
    [PD_IF2_A]       = ST(pd_if2_a),     [PD_IF3]         = ST(pd_if3),
    [PD_IF4]         = ST(pd_if4),       [PD_IF4_A]       = ST(pd_if4_a),
    [PD_BLDRULE0]    = ST(pd_bldrule0),  [PD_BLDRULE]     = ST(pd_bldrule),
    [PD_RULE1]       = ST(pd_rule1),     [PD_COMMAND]     = ST(pd_command),
    [PD_CMD_PREFIXED]= ST(pd_cmd_prefixed),[PD_SYMDEF]    = ST(pd_symdef),
    [PD_SYMDEF0]     = ST(pd_symdef0),   [PD_SYMDEF0A]    = ST(pd_symdef0a),
    [PD_SYMDEF0B]    = ST(pd_symdef0b),  [PD_SYMDEF1]     = ST(pd_symdef1),
    [PD_SYMDEF2]     = ST(pd_symdef2),   [PD_SYMDEF2A]    = ST(pd_symdef2a),
    [PD_SYMDEF3]     = ST(pd_symdef3),   [PD_SYMDEF3A]    = ST(pd_symdef3a),
    [PD_SYMDEF4]     = ST(pd_symdef4),   [PD_SYMDEF4A]    = ST(pd_symdef4a),
    [PD_SYMDEF5]     = ST(pd_symdef5),   [PD_SYMDEF5A]    = ST(pd_symdef5a),
    [PD_DEPEND]      = ST(pd_depend),    [PD_DEPEND0]     = ST(pd_depend0),
    [PD_DEPEND1]     = ST(pd_depend1),   [PD_DEPEND1A]    = ST(pd_depend1a),
    [PD_DEPEND1B]    = ST(pd_depend1b),  [PD_DEPEND2]     = ST(pd_depend2),
    [PD_PATHPFX]     = ST(pd_pathpfx),   [PD_PATHPFX1]    = ST(pd_pathpfx1),
    [PD_STRSYM]      = ST(pd_strsym),    [PD_SYMBOL]      = ST(pd_symbol),
    [PD_SYMBOL1]     = ST(pd_symbol1),   [PD_SYMREF]      = ST(pd_symref),
    [PD_SYMREF0]     = ST(pd_symref0),   [PD_SYMREF1]     = ST(pd_symref1),
    [PD_QUOTED]      = ST(pd_quoted),    [PD_DQUOTE]      = ST(pd_dquote),
    [PD_DQUOTE1]     = ST(pd_dquote1),   [PD_SQUOTE]      = ST(pd_squote),
    [PD_SQUOTE1]     = ST(pd_squote1),
};

const TPA_GRAMMAR mmk_parse_descrip_grammar = {
    TPA$K_GRAMMAR_MAGIC, PD_NSTATES, pd_states
};

/* ==========================================================================
 * PARSE_OBJECTS  (.mar lines 543-669)
 * ========================================================================== */

static const TPA_TRAN po_init[] = {             /* PO_INIT (545) */
    TR (TPA$_BLANK,  PO_INIT),
    TR (TPA$_EOS,    TPA$_EXIT),
    OTRA(TPA$_LAMBDA, PO_INIT1, PO_K_OBJ_INIT),
};
static const TPA_TRAN po_init1[] = {            /* PO_INIT1 (550) */
    OTRA(',',        PO_INIT,   PO_K_END_OBJ),
    OTRA(TPA$_EOS,   TPA$_EXIT, PO_K_END_OBJ),
    OTRA('~',        PO_INIT2,  PO_K_APPNAM_CMS),
    OTRA('(',        PO_LIB,    PO_K_LIB_BEGIN),
    OTRA(TPA$_BLANK, PO_ENDOBJ, PO_K_END_OBJ),
    OTRA(TPA$_ANY,   PO_INIT1,  PO_K_APPNAM),
};
static const TPA_TRAN po_init2[] = {            /* PO_INIT2 (558) */
    OTRA(',',        PO_INIT,   PO_K_END_OBJ),
    OTRA(TPA$_EOS,   TPA$_EXIT, PO_K_END_OBJ),
    OTR ('/',        PO_CMSGEN1),
    OTR (TPA$_BLANK, PO_INIT3),
};
static const TPA_TRAN po_init3[] = {            /* PO_INIT3 (564) */
    OTRA(',',        PO_INIT,   PO_K_END_OBJ),
    OTRA(TPA$_EOS,   TPA$_EXIT, PO_K_END_OBJ),
    OTR ('/',        PO_CMSGEN1),
    OTR (TPA$_BLANK, PO_INIT3),
    OTRA(TPA$_LAMBDA, PO_INIT,  PO_K_END_OBJ),
};
static const TPA_TRAN po_cmsgen1[] = {          /* PO_CMSGEN1 (571) */
    SUB(PO_GEN,      PO_CMSGEN2),
    OTR(TPA$_BLANK,  PO_CMSGEN1),
};
static const TPA_TRAN po_gen[]  = {             /* GENERATION (575) 'G' */
    OTR('G', TPA$K_NEXT_SEQ), OTR('g', TPA$K_NEXT_SEQ),
};
static const TPA_TRAN po_gen1[] = {             /* (578) 'E' */
    OTR('E', TPA$K_NEXT_SEQ), OTR('e', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen2[] = {             /* (582) 'N' */
    OTR('N', TPA$K_NEXT_SEQ), OTR('n', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen3[] = {             /* (586) 'E' */
    OTR('E', TPA$K_NEXT_SEQ), OTR('e', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen4[] = {             /* (590) 'R' */
    OTR('R', TPA$K_NEXT_SEQ), OTR('r', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen5[] = {             /* (594) 'A' */
    OTR('A', TPA$K_NEXT_SEQ), OTR('a', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen6[] = {             /* (598) 'T' */
    OTR('T', TPA$K_NEXT_SEQ), OTR('t', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen7[] = {             /* (602) 'I' */
    OTR('I', TPA$K_NEXT_SEQ), OTR('i', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen8[] = {             /* (606) 'O' */
    OTR('O', TPA$K_NEXT_SEQ), OTR('o', TPA$K_NEXT_SEQ), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_gen9[] = {             /* (610) 'N' */
    OTR('N', TPA$_EXIT), OTR('n', TPA$_EXIT), OTR(TPA$_LAMBDA, TPA$_EXIT),
};
static const TPA_TRAN po_cmsgen2[] = {          /* PO_CMSGEN2 (615) */
    OTR('=',        PO_CMSGEN3),
    OTR(TPA$_BLANK, PO_CMSGEN2),
};
static const TPA_TRAN po_cmsgen3[] = {          /* PO_CMSGEN3 (619) */
    OTR (TPA$_BLANK, PO_CMSGEN3),
    OTRA(TPA$_ANY,   PO_CMSGEN4, PO_K_APPGEN),
};
static const TPA_TRAN po_cmsgen4[] = {          /* PO_CMSGEN4 (623) */
    OTRA(',',        PO_INIT,   PO_K_END_OBJ),
    OTRA(TPA$_EOS,   TPA$_EXIT, PO_K_END_OBJ),
    OTRA(TPA$_BLANK, PO_INIT,   PO_K_END_OBJ),
    OTRA(TPA$_ANY,   PO_CMSGEN4, PO_K_APPGEN),
};
static const TPA_TRAN po_endobj[] = {           /* PO_ENDOBJ (629) */
    OTR(TPA$_BLANK,  PO_ENDOBJ),
    OTR(',',         PO_INIT),
    OTR(TPA$_EOS,    TPA$_EXIT),
    OTR(TPA$_LAMBDA, PO_INIT),
};
static const TPA_TRAN po_lib[] = {              /* PO_LIB (635) */
    OTR(TPA$_BLANK,  PO_LIB),
    OTR(TPA$_LAMBDA, TPA$K_NEXT_SEQ),
};
static const TPA_TRAN po_lib1[] = {             /* PO_LIB1 (639) */
    OTRA('=',        PO_LIBFILE, PO_K_MOD_FILE),
    OTRA(')',        PO_ENDOBJ,  PO_K_LIB_END),
    OTRA(',',        PO_LIB,     PO_K_MOD_END),
    OTR (TPA$_BLANK, PO_LIB2),
    OTRA(TPA$_ANY,   PO_LIB1,    PO_K_APPMOD),
};
static const TPA_TRAN po_lib2[] = {             /* PO_LIB2 (646) */
    OTRA('=',        PO_LIBFILE, PO_K_MOD_FILE),
    OTRA(',',        PO_LIB,     PO_K_MOD_END),
    OTRA(')',        PO_ENDOBJ,  PO_K_LIB_END),
    OTR (TPA$_BLANK, PO_LIB2),
    OTRA(TPA$_LAMBDA, PO_LIB1,   PO_K_MOD_END),
};
static const TPA_TRAN po_libfile[] = {          /* PO_LIBFILE (653) */
    OTR(TPA$_BLANK,  PO_LIBFILE),
    OTR(TPA$_LAMBDA, TPA$K_NEXT_SEQ),
};
static const TPA_TRAN po_libfil1[] = {          /* PO_LIBFIL1 (657) */
    OTRA(',',        PO_LIB,     PO_K_MOD_END),
    OTR (TPA$_BLANK, PO_LIBFIL2),
    OTRA(')',        PO_ENDOBJ,  PO_K_LIB_END),
    OTRA(TPA$_ANY,   PO_LIBFIL1, PO_K_APPFIL),
};
static const TPA_TRAN po_libfil2[] = {          /* PO_LIBFIL2 (663) */
    OTRA(',',        PO_LIB,     PO_K_MOD_END),
    OTRA(')',        PO_ENDOBJ,  PO_K_LIB_END),
    OTR (TPA$_BLANK, PO_LIBFIL2),
    OTRA(TPA$_LAMBDA, PO_LIB1,   PO_K_MOD_END),
};

static const TPA_STATE po_states[PO_NSTATES] = {
    [PO_INIT]     = ST(po_init),    [PO_INIT1]    = ST(po_init1),
    [PO_INIT2]    = ST(po_init2),   [PO_INIT3]    = ST(po_init3),
    [PO_CMSGEN1]  = ST(po_cmsgen1), [PO_GEN]      = ST(po_gen),
    [PO_GEN1]     = ST(po_gen1),    [PO_GEN2]     = ST(po_gen2),
    [PO_GEN3]     = ST(po_gen3),    [PO_GEN4]     = ST(po_gen4),
    [PO_GEN5]     = ST(po_gen5),    [PO_GEN6]     = ST(po_gen6),
    [PO_GEN7]     = ST(po_gen7),    [PO_GEN8]     = ST(po_gen8),
    [PO_GEN9]     = ST(po_gen9),    [PO_CMSGEN2]  = ST(po_cmsgen2),
    [PO_CMSGEN3]  = ST(po_cmsgen3), [PO_CMSGEN4]  = ST(po_cmsgen4),
    [PO_ENDOBJ]   = ST(po_endobj),  [PO_LIB]      = ST(po_lib),
    [PO_LIB1]     = ST(po_lib1),    [PO_LIB2]     = ST(po_lib2),
    [PO_LIBFILE]  = ST(po_libfile), [PO_LIBFIL1]  = ST(po_libfil1),
    [PO_LIBFIL2]  = ST(po_libfil2),
};

const TPA_GRAMMAR mmk_parse_objects_grammar = {
    TPA$K_GRAMMAR_MAGIC, PO_NSTATES, po_states
};
