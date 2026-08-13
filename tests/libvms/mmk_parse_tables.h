/*
 * mmk_parse_tables.h - interface to the C port of MMK's parse_tables.mar
 *                      (bead vms-486, self-host spine #2).
 *
 * The vendored MadGoat MMK ships its description-file / object-list grammars as
 * a 704-line MACRO-32 TPARSE state table (tests/corpus/tier3-mmk/parse_tables.mar,
 * third-party BSD-licensed freeware, NOT VSI/HPE/DEC source).  The OVMX contract
 * forbids building a MACRO-32 assembler, so that table is hand-ported here into
 * the OVMX TPA_GRAMMAR format and driven through the real lib$table_parse engine
 * (bead vms-9f6).  See mmk_parse_tables.c for the state-by-state port and the
 * faithfulness notes.
 *
 * CLEAN-ROOM (Rule 8): every state, transition, keyword and PRS_K_ / PO_K_ code
 * below is transcribed from the freeware .mar (behaviour + structure).  No VSI
 * source or binary was consulted; the TPA_GRAMMAR *layout* is an OVMX design
 * choice defined in tpadef.h.
 */

#ifndef __MMK_PARSE_TABLES_H
#define __MMK_PARSE_TABLES_H

#include <stdint.h>
#include "tpadef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Transition codes for PARSE_DESCRIP.  Values MUST match parse_tables.mar
 * (PRS_K_*, lines 110-167) — the real MMK PARSE_STORE dispatches on them.
 * -------------------------------------------------------------------------- */
enum {
    PRS_K_CHECK_COND    = 0,
    PRS_K_CMD_INIT      = 1,
    PRS_K_SYM_INIT      = 2,
    PRS_K_DEP_INIT      = 3,
    PRS_K_DIR_SFX       = 4,
    PRS_K_DIR_FIRST     = 5,
    PRS_K_DIR_LAST      = 6,
    PRS_K_RULE_INIT     = 7,
    PRS_K_DIR_RHS       = 8,
    PRS_K_RULE_NEWSFX   = 9,
    PRS_K_RULE_SFX      = 10,
    PRS_K_CMD_NOECHO    = 11,
    PRS_K_CMD_TEXT      = 12,
    PRS_K_SYM2DEP       = 13,
    PRS_K_SYM_VALUE     = 14,
    PRS_K_DEP_TRGAPP    = 15,
    PRS_K_DEP_RHS       = 16,
    PRS_K_RULE_END      = 17,
    PRS_K_DIR_IFDEF     = 18,
    PRS_K_DIR_ELSE      = 19,
    PRS_K_DIR_ENDIF     = 20,
    PRS_K_SYM2DEP2      = 21,
    PRS_K_CMD_IGNERR    = 22,
    PRS_K_DIR_SILENT    = 23,
    PRS_K_DIR_IGNORE    = 24,
    PRS_K_DIR_DEFAULT   = 25,
    PRS_K_DIR_INCLUDE   = 26,
    PRS_K_CMD_FFORCED   = 27,
    PRS_K_DEP_TRGAPP2   = 28,
    PRS_K_DEP_DC        = 29,
    PRS_K_RULE_INIPFX   = 30,
    PRS_K_RULE_NEWPFX   = 31,
    PRS_K_DIR_IFLHS     = 32,
    PRS_K_DIR_IFEQL     = 33,
    PRS_K_DIR_IFNEQ     = 34,
    PRS_K_DIR_IFRHS     = 35,
    PRS_K_CMD_LFORCED   = 36,
    PRS_K_CMD_SETFLAGS  = 37,
    PRS_K_DIR_IFNDEF    = 38,
    PRS_K_DIR_IFGEQ     = 39,
    PRS_K_DIR_IFLEQ     = 40,
    PRS_K_DIR_IFGTR     = 41,
    PRS_K_DIR_IFLSS     = 42,
    PRS_K_DIR_NOT       = 43,
    PRS_K_DIR_AND       = 44,
    PRS_K_DIR_OR        = 45,
    PRS_K_DIR_BUILTIN   = 46,
    PRS_K_DIR_CASE      = 47,
    PRS_K_DIR_ELSIF     = 48,
    PRS_K_CHECK_GNU     = 49,
    PRS_K_DIR_GNU       = 50,
    PRS_K_SYM_DEFINED   = 51,
    PRS_K_SYM_APPEND    = 52,
    PRS_K_SYM_DO        = 53,
    PRS_K_SYM_EVAL      = 54,
    PRS_K_DIR_SFX_AFTER = 55,
    PRS_K_DIR_SFX_BEFORE= 56,
    PRS_K_DIR_SFX_DELETE= 57
};

/* --------------------------------------------------------------------------
 * Transition codes for PARSE_OBJECTS (PO_K_*, .mar lines 172-182).
 * -------------------------------------------------------------------------- */
enum {
    PO_K_LIB_BEGIN   = 1,
    PO_K_END_OBJ     = 2,
    PO_K_APPNAM      = 3,
    PO_K_APPNAM_CMS  = 4,
    PO_K_LIB_END     = 5,
    PO_K_MOD_END     = 6,
    PO_K_APPMOD      = 7,
    PO_K_APPFIL      = 8,
    PO_K_OBJ_INIT    = 9,
    PO_K_MOD_FILE    = 10,
    PO_K_APPGEN      = 11
};

/* --------------------------------------------------------------------------
 * Recording TPARSE block.  MMK extends the standard TPADEF with its own TPABLK
 * fields; here the extension records every action that fired, so a test can
 * assert what the ported state machine actually did.  The TPADEF MUST be the
 * first member (lib$table_parse is handed this block by address).
 * -------------------------------------------------------------------------- */
#define MMK_MAX_EVENTS 512
#define MMK_TOKMAX      96

typedef struct {
    TPADEF   tpa;                          /* standard block — MUST be first  */
    int      gnu_mode;                     /* CHECK_GNU gate (0 => not GNU)    */
    int      cond_skip;                    /* CHECK_COND gate (0 => not skip)  */
    int      nevents;                      /* number of actions that fired     */
    uint32_t code[MMK_MAX_EVENTS];         /* the PRS_K_ / PO_K_ code of each   */
    char     tok [MMK_MAX_EVENTS][MMK_TOKMAX]; /* the token span at each fire  */
} mmk_parse_ctx;

/* The two ported grammars (defined in mmk_parse_tables.c). */
extern const TPA_GRAMMAR mmk_parse_descrip_grammar;   /* $INIT_STATE PARSE_STATE */
extern const TPA_GRAMMAR mmk_parse_objects_grammar;   /* $INIT_STATE PO_STATE    */

#ifdef __cplusplus
}
#endif

#endif /* __MMK_PARSE_TABLES_H */
