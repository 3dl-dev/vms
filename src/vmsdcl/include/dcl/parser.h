#ifndef __DCL_PARSER_H
#define __DCL_PARSER_H

#include <stdint.h>

/* Maximum lengths */
#define DCL_MAX_LINE    4096
#define DCL_MAX_LABEL   255
#define DCL_MAX_SYMBOL  255
#define DCL_MAX_VALUE   4096
#define DCL_MAX_PARAMS  8
/* list elements a command can carry: P1-P8 plus the overflow (rd vms-ba2) */
#define DCL_MAX_ELEMS   (DCL_MAX_PARAMS + DCL_MAX_LINE / 2)

/* Command node types */
typedef enum {
    DCL_NODE_COMMAND,
    DCL_NODE_ASSIGN,       /* symbol = value */
    DCL_NODE_IF,           /* IF ... THEN ... */
    DCL_NODE_GOTO,
    DCL_NODE_GOSUB,
    DCL_NODE_RETURN,
    DCL_NODE_LABEL,        /* LABEL: */
    DCL_NODE_PIPE,         /* PIPE cmd1 | cmd2 */
    DCL_NODE_COMMENT,      /* $! comment */
} dcl_node_type;

/* A parsed qualifier */
struct dcl_qualifier {
    char name[64];
    char value[256];       /* Value after = if present */
    int  negated;          /* /NOQUALIFIER */
    int  present;
};

/* A parsed command */
struct dcl_command {
    dcl_node_type type;
    char verb[64];                          /* Command verb */
    char params[DCL_MAX_PARAMS][DCL_MAX_VALUE]; /* Positional parameters */
    int  param_count;
    struct dcl_qualifier qualifiers[32];    /* /QUAL=value */
    int  qualifier_count;
    char label[DCL_MAX_LABEL];             /* Label if present */
    char rest[DCL_MAX_LINE];               /* Unparsed rest of line */
    char raw_tail[DCL_MAX_LINE];           /* Raw arg tail after the verb, kept
                                            * separately from `rest` so it can be
                                            * delivered verbatim as a foreign
                                            * command's argv without disturbing
                                            * the `rest` consumers (SPAWN, PIPE,
                                            * assignments). vms-615. */
    /* rd vms-465: list elements past P8 (a comma list longer than the eight
     * parameter slots, e.g. LIBRARY lib a,b,c,... from GCC's ar), NUL-separated
     * in order. A built-in that takes a list reads them through dcl_param_total
     * / dcl_param_at (the P1-list file verbs, rd vms-ba2, and LIBRARY); every
     * other built-in answers %DCL-W-MAXPARM rather than silently dropping them. overflow_lost: one did not fit (also MAXPARM). */
    char overflow[DCL_MAX_LINE];
    int  overflow_count;
    int  overflow_len;
    int  overflow_lost;
    /* rd vms-ba2: the parameter (0 = P1) each element of params[] + overflow
     * belongs to. Elements joined by ',' are one parameter's list; a blank
     * starts the next parameter. slot_count = parameters given. */
    unsigned char elem_slot[DCL_MAX_ELEMS];
    int  slot_recorded;
    int  slot_count;
    int  list_elem;      /* rd vms-ba2: index of params[0] in its P1 list (0 = first) */
    uint32_t parse_status;                 /* rd vms-0b0: a parameter error the
                                            * parser found (CLI$_PARMDEL for a '^'
                                            * under PARSE_STYLE=TRADITIONAL),
                                            * reported when a built-in verb runs */
};

/* All positional parameters including list elements past P8 (rd vms-465). */
int dcl_param_total(const struct dcl_command *cmd);
const char *dcl_param_at(const struct dcl_command *cmd, int i);
/* Parameters given (P1..Pn), counting a comma list as one (rd vms-ba2), and
 * the parameter (0 = P1) element i belongs to. */
int dcl_slot_total(const struct dcl_command *cmd);
int dcl_param_slot(const struct dcl_command *cmd, int i);
/* A list element without device/directory takes the previous element's
 * (DCL's sticky defaults in a list); and "spec names one file" (no wildcard,
 * not a bare directory/device). rd vms-ba2, dcl_exec.c. */
void dcl_plist_sticky(const char *prev, const char *cur, char *out, size_t cap);
int dcl_spec_names_one_file(const char *s);

/* Parse a DCL command line into a command structure */
int dcl_parse_line(const char *line, struct dcl_command *cmd);

/* Check if a qualifier is present in a command */
int dcl_has_qualifier(const struct dcl_command *cmd, const char *name);

/* Get qualifier value */
const char *dcl_qualifier_value(const struct dcl_command *cmd, const char *name);

/* Match a command name with minimum-uniqueness abbreviation */
int dcl_match_command(const char *abbrev, const char *full, int min_len);

#endif /* __DCL_PARSER_H */
