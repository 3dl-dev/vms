#ifndef __DCL_CDU_H
#define __DCL_CDU_H

/*
 * CDU - Command Definition Utility structures
 *
 * Defines the structure of DCL commands for parsing and dispatch.
 */

#include <stdint.h>

/* Maximum abbreviation - minimum unique prefix length */
#define CDU_MIN_ABBREV 4

/* Command flags */
#define CDU_F_ABBREV     0x01  /* Allow abbreviation */
#define CDU_F_QUALIFIER  0x02  /* Has qualifiers */
#define CDU_F_PARAM      0x04  /* Takes parameters */
#define CDU_F_NOPREFIX   0x08  /* Don't require $ prefix */

/* Forward declaration */
struct dcl_command;

/* Command handler function type */
typedef int (*dcl_handler_fn)(struct dcl_command *cmd);

/*
 * Qualifier value-type (CDU/CLD "VALUE" clause). Grounded in the public VSI
 * OpenVMS Command Definition Utility manual (the .CLD "qualifier" statement's
 * VALUE(...) attribute) and the DCL Dictionary per-command qualifier
 * descriptions -- NOT invented (project Rule 8).
 */
typedef enum {
    CDU_VT_NONE = 0,   /* /QUAL              -- flag, no value permitted */
    CDU_VT_VALUE,      /* /QUAL=value        -- a single value */
    CDU_VT_LIST,       /* /QUAL=(v1,v2,...)  -- a parenthesised value list */
    CDU_VT_KEYWORD,    /* /QUAL=keyword      -- value drawn from a keyword set */
} cdu_value_type;

/* Per-qualifier flags */
#define CDU_Q_NEGATABLE  0x01  /* the /NOxxx form is legal (CDU NEGATABLE) */
#define CDU_Q_VALREQ     0x02  /* a value is REQUIRED (CDU VALUE(REQUIRED)) */
#define CDU_Q_DEFAULT    0x04  /* qualifier is on by default (CDU DEFAULT) */

/*
 * A single qualifier declaration for a verb. This is the missing piece the
 * fidelity audit (docs/design-dcl-fidelity.md sec 2) named: struct dcl_verb
 * previously carried only the boolean CDU_F_QUALIFIER, so parse_qualifier()
 * had nothing to validate against and %DCL-W-IVQUAL was structurally
 * unreachable. A verb that declares a NULL-terminated array of these gets
 * uniform IVQUAL / IVKEYW / VALREQ validation for free.
 */
struct dcl_qual_def {
    const char           *name;      /* qualifier name, UPPERCASE, canonical */
    cdu_value_type        vtype;     /* value-type accepted */
    uint32_t              qflags;    /* CDU_Q_* */
    const char *const    *keywords;  /* NULL-terminated keyword set for
                                      * CDU_VT_KEYWORD; NULL otherwise */
    const char           *deflt;     /* default value string, or NULL */
};

/* Command definition */
struct dcl_verb {
    const char     *name;       /* Verb name (uppercase) */
    dcl_handler_fn  handler;    /* Handler function */
    uint32_t        flags;
    int             min_abbrev; /* Minimum abbreviation length */
    const char     *help;       /* Brief help text */
    const struct dcl_qual_def *quals; /* NULL-terminated qualifier table, or
                                       * NULL = verb not yet retrofit with a
                                       * table (legacy accept-all; IVQUAL stays
                                       * unreachable for it until populated). */
};

/* Find a command by name (with abbreviation matching) */
const struct dcl_verb *dcl_find_verb(const char *name);

/* Register the built-in command table */
void dcl_register_builtins(void);

/* Get the command table for help listing */
const struct dcl_verb *dcl_get_verb_table(int *count);

/*
 * Validate a parsed command's qualifiers against the verb's declared
 * qualifier table. Emits the authentic %DCL-W-IVQUAL / %DCL-W-IVKEYW /
 * %DCL-W-VALREQ message and returns the matching VMS status on the first
 * violation; returns SS$_NORMAL if every qualifier is legal, or if the verb
 * declares no table (verb->quals == NULL, legacy accept-all). Also
 * canonicalises abbreviated qualifier names in-place so handlers that read
 * qualifiers by full name keep working. Called from dispatch BEFORE the
 * verb's handler runs (docs/design-dcl-fidelity.md sec 4).
 */
int dcl_validate_qualifiers(const struct dcl_verb *verb,
                            struct dcl_command *cmd);

#endif /* __DCL_CDU_H */
