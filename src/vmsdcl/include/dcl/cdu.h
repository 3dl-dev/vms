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

/* Command definition */
struct dcl_verb {
    const char     *name;       /* Verb name (uppercase) */
    dcl_handler_fn  handler;    /* Handler function */
    uint32_t        flags;
    int             min_abbrev; /* Minimum abbreviation length */
    const char     *help;       /* Brief help text */
};

/* Find a command by name (with abbreviation matching) */
const struct dcl_verb *dcl_find_verb(const char *name);

/* Register the built-in command table */
void dcl_register_builtins(void);

/* Get the command table for help listing */
const struct dcl_verb *dcl_get_verb_table(int *count);

#endif /* __DCL_CDU_H */
