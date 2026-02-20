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

#ifdef __cplusplus
}
#endif

#endif /* __TPADEF_H */
