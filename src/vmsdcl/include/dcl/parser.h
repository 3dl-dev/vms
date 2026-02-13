#ifndef __DCL_PARSER_H
#define __DCL_PARSER_H

#include <stdint.h>

/* Maximum lengths */
#define DCL_MAX_LINE    4096
#define DCL_MAX_LABEL   255
#define DCL_MAX_SYMBOL  255
#define DCL_MAX_VALUE   4096
#define DCL_MAX_PARAMS  8

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
};

/* Parse a DCL command line into a command structure */
int dcl_parse_line(const char *line, struct dcl_command *cmd);

/* Check if a qualifier is present in a command */
int dcl_has_qualifier(const struct dcl_command *cmd, const char *name);

/* Get qualifier value */
const char *dcl_qualifier_value(const struct dcl_command *cmd, const char *name);

/* Match a command name with minimum-uniqueness abbreviation */
int dcl_match_command(const char *abbrev, const char *full, int min_len);

#endif /* __DCL_PARSER_H */
