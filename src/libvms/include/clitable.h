/*
 * CLITABLE.H - Compiled command-table (CLD) representation for the CLI$
 *              callable interface (cli$dcl_parse / cli$present / cli$get_value).
 *
 * OpenVMX compatibility layer.
 *
 * ------------------------------------------------------------------------
 * PROVENANCE / CLEAN-ROOM NOTE (project Rule 8)
 * ------------------------------------------------------------------------
 * The FIELD SEMANTICS modelled here -- verbs, positional parameters
 * (P1..Pn, each with a LABEL), qualifiers with the VALUE(...) attribute
 * (single value, LIST, REQUIRED, DEFAULT="...", TYPE=keyword-set), the
 * NEGATABLE/NONNEGATABLE and DEFAULT qualifier attributes, and keyword
 * value sets (DEFINE TYPE ... KEYWORD ...) -- are all derived from the
 * PUBLIC VSI OpenVMS Command Definition Utility (CDU) manual and the DCL
 * Dictionary descriptions of the CLI$ routines. Nothing here is copied or
 * paraphrased from VSI/HPE/DEC source or headers.
 *
 * The IN-MEMORY BYTE LAYOUT of the structures below (what a "compiled CLD"
 * looks like once produced) is an *** OVMX DESIGN CHOICE ***. VSI does not
 * publish the byte-level format of the object module that SET COMMAND emits,
 * so per Rule 8 OVMX defines its own representation and labels it as such --
 * it is NOT presented as VMS-authentic. Consumers (e.g. MMK) only ever hold
 * an opaque pointer to a struct cli_command_table (their "MMK_CLD"); they do
 * not depend on the layout.
 * ------------------------------------------------------------------------
 */

#ifndef __CLITABLE_H
#define __CLITABLE_H

#include <stdint.h>
#include <stddef.h>
#include "descrip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Magic word stamped into a compiled command table (OVMX design choice). */
#define CLI_TABLE_MAGIC 0x434C4454u /* 'CLDT' */

/* ---- qualifier / parameter attribute flags (from CDU VALUE(...) etc.) ---- */
#define CLI_A_NEGATABLE 0x0001u /* the /NOxxx form is legal (NEGATABLE)        */
#define CLI_A_VALUE     0x0002u /* accepts a value at all (VALUE(...))         */
#define CLI_A_VALREQ    0x0004u /* a value is REQUIRED  (VALUE(REQUIRED))      */
#define CLI_A_LIST      0x0008u /* accepts a value list (VALUE(LIST))          */
#define CLI_A_DEFAULT   0x0010u /* present by default   (qualifier DEFAULT)    */
#define CLI_A_KEYWORD   0x0020u /* value drawn from a keyword set (TYPE=type)  */
#define CLI_A_FILE      0x0040u /* VALUE(TYPE=$FILE) -- treated as free text   */

/* One keyword in a DEFINE TYPE keyword set. */
struct cli_keyword {
    const char *name;   /* UPPERCASE canonical keyword name */
    uint32_t    flags;  /* CLI_A_DEFAULT / CLI_A_NEGATABLE within the set */
};

/* A qualifier declaration (CDU QUALIFIER statement). */
struct cli_qual {
    const char               *name;     /* UPPERCASE canonical qualifier name */
    uint32_t                  flags;    /* CLI_A_* */
    const char               *deflt;    /* VALUE(DEFAULT="x") string, or NULL */
    const struct cli_keyword *keywords; /* NULL-terminated keyword set, or NULL */
};

/* A positional parameter declaration (CDU PARAMETER statement). */
struct cli_param {
    const char *label;  /* LABEL= name (UPPERCASE), e.g. "TARGET"; addressable
                         * by cli$present/cli$get_value */
    uint32_t    flags;  /* CLI_A_LIST / CLI_A_VALREQ */
};

/* A verb declaration (CDU DEFINE VERB statement). */
struct cli_verb {
    const char             *name;     /* UPPERCASE verb name */
    const struct cli_param *params;   /* array, length nparams */
    int                     nparams;
    const struct cli_qual  *quals;    /* array, length nquals */
    int                     nquals;
};

/*
 * The compiled command table -- what a caller passes to cli$dcl_parse as its
 * "command table" argument (MMK's MMK_CLD). OVMX design choice (see header
 * provenance note).
 */
struct cli_command_table {
    uint32_t                magic;       /* CLI_TABLE_MAGIC */
    const struct cli_verb  *verbs;       /* array, length nverbs */
    int                     nverbs;
    void                   *ovmx_arena;  /* heap arena when produced by
                                          * cli$compile_cld; NULL for a static
                                          * (hand-authored) table */
};

/* ================================================================
 * CLI$ callable interface (public VSI DCL Dictionary semantics).
 *
 * cli$dcl_parse - parse a command string against a compiled command
 *   table, establishing it as the "current command" for subsequent
 *   cli$present / cli$get_value calls. The prompt/param routines have the
 *   lib$get_input signature; they are invoked to obtain a required
 *   parameter that was omitted from the command string.
 *
 * cli$present   - test whether an entity (qualifier, parameter label, or
 *   "QUAL.KEYWORD") appeared. Returns CLI$_PRESENT / CLI$_ABSENT /
 *   CLI$_NEGATED / CLI$_DEFAULTED (from libclidef.h). PRESENT and DEFAULTED
 *   have success severity; ABSENT and NEGATED do not.
 *
 * cli$get_value - retrieve the next value of an entity. Returns CLI$_COMMA
 *   (success) when a further list value follows, SS$_NORMAL on the last
 *   value, or CLI$_ABSENT (failure) when no value is available.
 * ================================================================ */

typedef uint32_t (*cli_prompt_rtn)(struct dsc$descriptor_s *result,
                                   const struct dsc$descriptor_s *prompt,
                                   uint16_t *result_len);

uint32_t cli$dcl_parse(const struct dsc$descriptor_s *command,
                       const struct cli_command_table *table,
                       cli_prompt_rtn param_rtn,
                       cli_prompt_rtn prompt_rtn);

uint32_t cli$present(const struct dsc$descriptor_s *label);

uint32_t cli$get_value(const struct dsc$descriptor_s *label,
                       struct dsc$descriptor_s *value,
                       uint16_t *retlen);

/* ================================================================
 * cli$compile_cld / cli$free_cld -- OVMX EXTENSION (not a VSI routine).
 *
 * The "compiled-CLD support" for the toolchain: parse CLD source text (the
 * CDU command-definition language: MODULE/IDENT/DEFINE TYPE/DEFINE VERB/
 * PARAMETER/QUALIFIER/KEYWORD) into a heap-allocated struct cli_command_table
 * that cli$dcl_parse consumes. This is the OVMX analogue of SET COMMAND
 * compiling a .CLD into an object module. Labelled an OVMX design choice.
 *
 * On success *table_out points to a table that MUST be released with
 * cli$free_cld. Returns a VMS status code.
 * ================================================================ */
uint32_t cli$compile_cld(const struct dsc$descriptor_s *cld_source,
                         struct cli_command_table **table_out);

void cli$free_cld(struct cli_command_table *table);

#ifdef __cplusplus
}
#endif

#endif /* __CLITABLE_H */
