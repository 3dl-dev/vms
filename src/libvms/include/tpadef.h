/*
 * TPADEF.H - VMS LIB$TABLE_PARSE (TPARSE) Block Definitions
 *
 * OpenVMX compatibility layer - Defines the TPADEF structure and TPA$_
 * constants used with lib$table_parse (LIB$TPARSE in MACRO-32) for
 * finite state table parsing.
 *
 * The TPARSE block (TPA block) passes state and input string information
 * between lib$table_parse and the caller-supplied state and key tables.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$TABLE_PARSE
 *            OpenVMS MACRO-32 Programming Manual — $TPADEF
 */

#ifndef __TPADEF_H
#define __TPADEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * TPADEF — TPARSE (table parse) control block
 *
 * The TPADEF block is passed by address to lib$table_parse.
 * The caller initializes tpa$l_count, tpa$l_options, tpa$l_stringcnt,
 * and tpa$l_stringptr before each call.
 * ================================================================ */

struct _tpadef {
    uint32_t tpa$l_count;       /* Must be TPA$K_COUNT0 (8) */
    uint32_t tpa$l_options;     /* Options (TPA$M_BLANKS, etc.) */
    uint32_t tpa$l_stringcnt;   /* Remaining character count in input string */
    char    *tpa$l_stringptr;   /* Pointer to next character to parse */
    uint32_t tpa$l_tokencnt;    /* Length of most recent token matched */
    char    *tpa$l_tokenptr;    /* Pointer to start of most recent token */
    uint32_t tpa$l_number;      /* Numeric value of most recent numeric token */
    uint32_t tpa$l_param;       /* User parameter (passed from state table) */
    uint8_t  tpa$b_char;        /* First character of the most recent token match
                                 * (VMS TPARSE exposes the matched character here;
                                 * MMK reads it to build suffix rules char-by-char). */
};

typedef struct _tpadef TPADEF;

/* ================================================================
 * TPA$K_ — TPARSE block size constants
 *
 * tpa$l_count must be initialized to TPA$K_COUNT0 to indicate
 * the block version/size to lib$table_parse.
 * ================================================================ */

#define TPA$K_COUNT0        8   /* Number of longwords in TPADEF block */
#define TPA$K_VERSION0      0   /* Block version 0 */

/* ================================================================
 * TPA$M_ — Option bit masks for tpa$l_options
 * ================================================================ */

#define TPA$M_BLANKS        0x00000001  /* Ignore blanks between tokens */
#define TPA$M_ABBREV        0x00000002  /* Allow keyword abbreviation */

/* ================================================================
 * TPA$_ — Special state table token codes
 *
 * These are used in MACRO-32 state tables to match special
 * classes of input characters.
 * ================================================================ */

#define TPA$_LAMBDA         0xFFFFFFF0  /* Zero-length match (always succeeds) */
#define TPA$_EXIT           0xFFFFFFF1  /* Successful parse completion */
#define TPA$_FAIL           0xFFFFFFF2  /* Force parse failure */
#define TPA$_OCTAL          0xFFFFFFF3  /* Match an octal number */
#define TPA$_DECIMAL        0xFFFFFFF4  /* Match a decimal number */
#define TPA$_HEX            0xFFFFFFF5  /* Match a hexadecimal number */
#define TPA$_SYMBOL         0xFFFFFFF6  /* Match an alphanumeric symbol */
#define TPA$_STRING         0xFFFFFFF7  /* Match any string to end of input */
#define TPA$_BLANK          0xFFFFFFF8  /* Match one or more blanks */
#define TPA$_ANY            0xFFFFFFF9  /* Match any single character */
#define TPA$_EOS            0xFFFFFFFA  /* Match end of string */
#define TPA$_ALPHA          0xFFFFFFFB  /* Match one or more alpha chars */
#define TPA$_DIGIT          0xFFFFFFFC  /* Match one or more digit chars */
#define TPA$_UIC            0xFFFFFFFD  /* Match a UIC [g,m] */

/* ================================================================
 * OVMX C TABLE FORMAT for lib$table_parse / lib$tparse
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) ***
 *
 * The public VSI OpenVMS RTL documentation specifies the SEMANTICS of
 * LIB$TABLE_PARSE (the state/transition finite-state parser, the TPA$
 * special token classes above, the action-routine argument block) but
 * does NOT publish the byte-level layout of the MACRO-32 $STATE/$TRAN
 * state and key tables produced by the STARLET table-definition macros.
 *
 * Per Rule 8, where the layout is not published OVMX defines its OWN
 * representation and labels it as an OVMX design choice — it is NOT
 * presented as the VMS-authentic on-disk table format.  The structures
 * below are that representation.  The real engine in lib_tparse.c
 * consumes them; the parse_tables.mar -> C port (bead vms-486) EMITS
 * them.  The observable BEHAVIOUR (which strings parse, which fail,
 * which action routines fire with which token/param) is what matches
 * VMS — not the table bytes.
 * ================================================================ */

/*
 * Action-routine callback.  VMS LIB$TABLE_PARSE passes the ADDRESS of the
 * TPARSE argument block (not the eight fields as separate arguments — the
 * distinction LIB$TABLE_PARSE draws from the older LIB$TPARSE call).  The
 * caller may extend the block past the standard TPADEF (see MMK's TPABLK);
 * the engine only touches the standard fields, so the callback gets the
 * ORIGINAL block pointer and can see its own extension fields.
 *
 * Return an odd (success) VMS status to ACCEPT the transition, or an even
 * (failure) status to REJECT it — on reject the engine backtracks the
 * input position and tries the next transition in the state (this is how
 * MMK's PRS_K_CHECK_GNU / PRS_K_CHECK_COND gate transitions).
 */
typedef uint32_t (*tpa_action_t)(void *tparse_block);

/*
 * Transition "type" markers that are neither a literal character (0..255)
 * nor a TPA$_ special class (0xFFFFFFF0..).  OVMX design choice.
 */
#define TPA$K_KEYWORD       0xFFFFFF00u /* type: match keyword string (tran.keyword) */
#define TPA$K_SUBEXPR       0xFFFFFF01u /* type: match subexpression at state tran.target */

/*
 * "next state" sentinels for TPA_TRAN.next, alongside a plain state index.
 * TPA$_EXIT / TPA$_FAIL (above) double as next-state values: EXIT ends the
 * parse successfully, FAIL forces LIB$_SYNTAXERR.
 */
#define TPA$K_NEXT_SEQ      0xFFFFFF02u /* next: fall through to the next state in the array */

/* One transition (one $TRAN). */
struct tpa_tran {
    uint32_t      type;    /* char 0..255, TPA$_ class, TPA$K_KEYWORD, or TPA$K_SUBEXPR */
    const char   *keyword; /* keyword string when type==TPA$K_KEYWORD (else NULL) */
    uint32_t      target;  /* subexpression start state when type==TPA$K_SUBEXPR */
    uint32_t      next;    /* next state index, or TPA$_EXIT/TPA$_FAIL/TPA$K_NEXT_SEQ */
    tpa_action_t  action;  /* action routine, or NULL */
    uint32_t      param;   /* stored into tpa$l_param and passed to the action */
};
typedef struct tpa_tran TPA_TRAN;

/* One state (one $STATE): an ordered list of transitions tried in turn. */
struct tpa_state {
    const TPA_TRAN *trans;
    uint32_t        ntrans;
};
typedef struct tpa_state TPA_STATE;

/*
 * The grammar passed by address as lib$table_parse's state_table argument.
 * states[0] is the start state ($INIT_STATE's first $STATE).  The key_table
 * argument is RESERVED in the OVMX format (may be NULL); the whole grammar
 * travels in state_table.
 */
#define TPA$K_GRAMMAR_MAGIC 0x54504147u /* 'TPAG' — guards against a bad pointer */
struct tpa_grammar {
    uint32_t         magic;    /* must be TPA$K_GRAMMAR_MAGIC */
    uint32_t         nstates;
    const TPA_STATE *states;   /* states[0] == start state */
};
typedef struct tpa_grammar TPA_GRAMMAR;

#ifdef __cplusplus
}
#endif

#endif /* __TPADEF_H */
