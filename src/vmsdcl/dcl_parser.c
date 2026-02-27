/*
 * dcl_parser.c - Hand-written recursive descent parser for DCL
 *
 * Parses a DCL command line into a dcl_command structure.
 * Handles: commands, qualifiers, parameters, assignments,
 * labels, @procedure, and IF/THEN/ELSE constructs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "dcl/lexer.h"
#include "dcl/parser.h"

/*
 * Convert a string to uppercase in-place.
 */
static void str_upcase(char *s)
{
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

/*
 * Copy src to dst, uppercasing, up to max-1 chars.
 */
static void str_upcase_copy(char *dst, const char *src, size_t max)
{
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

/*
 * Match a (possibly abbreviated) command name against a full command name.
 * Returns 1 if 'abbrev' matches 'full' with at least 'min_len' characters.
 */
int dcl_match_command(const char *abbrev, const char *full, int min_len)
{
    size_t alen = strlen(abbrev);
    size_t flen = strlen(full);

    if ((int)alen < min_len) return 0;
    if (alen > flen) return 0;

    for (size_t i = 0; i < alen; i++) {
        if (toupper((unsigned char)abbrev[i]) != toupper((unsigned char)full[i]))
            return 0;
    }
    return 1;
}

/*
 * Parse a qualifier from the token stream.
 * The leading / has already been consumed by the lexer, which returns
 * TOK_QUALIFIER with the qualifier name in token->value.
 */
static int parse_qualifier(dcl_lexer_t *lex, dcl_token_t *tok,
                           struct dcl_command *cmd)
{
    if (cmd->qualifier_count >= 32) return -1;

    struct dcl_qualifier *q = &cmd->qualifiers[cmd->qualifier_count];
    memset(q, 0, sizeof(*q));
    q->present = 1;

    /* Check for /NOQUALIFIER (negated) */
    if (strncasecmp(tok->value, "NO", 2) == 0 && strlen(tok->value) > 2) {
        /* Could be a negated qualifier or a qualifier starting with NO.
         * Heuristic: if the rest is >= 2 chars, treat as negated. */
        q->negated = 1;
        str_upcase_copy(q->name, tok->value + 2, sizeof(q->name));
    } else {
        str_upcase_copy(q->name, tok->value, sizeof(q->name));
    }

    /* Check for =VALUE after the qualifier */
    dcl_token_t peek;
    if (dcl_lexer_peek(lex, &peek) == 0 && peek.type == TOK_EQUALS) {
        dcl_lexer_next(lex, &peek); /* consume = */

        /* Read the value: could be a word, string, number, or parenthesized list */
        dcl_token_t val;
        if (dcl_lexer_peek(lex, &val) == 0) {
            if (val.type == TOK_LPAREN) {
                /* Parenthesized list: /QUAL=(val1,val2,...) */
                dcl_lexer_next(lex, &val); /* consume ( */
                size_t vi = 0;
                q->value[0] = '(';
                vi = 1;
                int depth = 1;
                while (depth > 0) {
                    dcl_token_t item;
                    if (dcl_lexer_next(lex, &item) != 0) break;
                    if (item.type == TOK_LPAREN) depth++;
                    else if (item.type == TOK_RPAREN) {
                        depth--;
                        if (depth == 0) break;
                    }
                    if (item.type == TOK_EOF) break;
                    /* Append to value */
                    size_t slen = strlen(item.value);
                    if (vi + slen < sizeof(q->value) - 2) {
                        memcpy(q->value + vi, item.value, slen);
                        vi += slen;
                    }
                }
                if (vi < sizeof(q->value) - 1) q->value[vi++] = ')';
                q->value[vi] = '\0';
            } else if (val.type == TOK_WORD || val.type == TOK_STRING ||
                       val.type == TOK_NUMBER) {
                dcl_lexer_next(lex, &val); /* consume value */
                strncpy(q->value, val.value, sizeof(q->value) - 1);
                q->value[sizeof(q->value) - 1] = '\0';
            }
        }
    }

    cmd->qualifier_count++;
    return 0;
}

/*
 * Collect the rest of the line as a single string (for assignments, IF, etc.)
 */
static void collect_rest(dcl_lexer_t *lex, char *buf, size_t bufsize)
{
    /* Just copy the remaining input */
    size_t remaining = lex->length - lex->pos;
    if (remaining >= bufsize) remaining = bufsize - 1;
    if (remaining > 0) {
        memcpy(buf, lex->input + lex->pos, remaining);
    }
    buf[remaining] = '\0';

    /* Trim trailing whitespace */
    while (remaining > 0 && (buf[remaining - 1] == ' ' ||
           buf[remaining - 1] == '\t' || buf[remaining - 1] == '\n')) {
        buf[--remaining] = '\0';
    }

    /* Advance lexer to end */
    lex->pos = lex->length;
}

/*
 * Parse a DCL command line.
 *
 * DCL syntax:
 *   [LABEL:] VERB [/QUAL[=val]] [param] [param] ...
 *   SYMBOL = value
 *   SYMBOL := value
 *   SYMBOL :== value
 *   SYMBOL == value
 *   @filespec [params...]
 *   IF condition THEN command
 *   $! comment
 *
 * Returns 0 on success, -1 on error.
 */
int dcl_parse_line(const char *line, struct dcl_command *cmd)
{
    dcl_lexer_t lex;
    dcl_token_t tok;

    if (!line || !cmd) return -1;

    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DCL_NODE_COMMAND;

    dcl_lexer_init(&lex, line);

    /* Get first token */
    if (dcl_lexer_next(&lex, &tok) != 0) return -1;

    /* Skip empty line or EOF */
    if (tok.type == TOK_EOF || tok.type == TOK_NEWLINE) {
        cmd->type = DCL_NODE_COMMENT;
        return 0;
    }

    /* Check for @ (procedure invocation) */
    if (tok.type == TOK_AT) {
        /* Next token is the filename */
        dcl_token_t file_tok;
        if (dcl_lexer_next(&lex, &file_tok) == 0 &&
            (file_tok.type == TOK_WORD || file_tok.type == TOK_STRING)) {
            str_upcase_copy(cmd->verb, "@", sizeof(cmd->verb));
            strncpy(cmd->params[0], file_tok.value, sizeof(cmd->params[0]) - 1);
            cmd->param_count = 1;
            /* Collect remaining params */
            while (cmd->param_count < DCL_MAX_PARAMS) {
                dcl_token_t p;
                if (dcl_lexer_next(&lex, &p) != 0) break;
                if (p.type == TOK_EOF || p.type == TOK_NEWLINE) break;
                if (p.type == TOK_WORD || p.type == TOK_STRING ||
                    p.type == TOK_NUMBER) {
                    strncpy(cmd->params[cmd->param_count], p.value,
                            sizeof(cmd->params[0]) - 1);
                    cmd->param_count++;
                }
            }
        }
        return 0;
    }

    /* Check for label: first token is LABEL type */
    if (tok.type == TOK_LABEL) {
        str_upcase_copy(cmd->label, tok.value, sizeof(cmd->label));
        cmd->type = DCL_NODE_LABEL;
        /* Get the next token (command after label, if any) */
        if (dcl_lexer_next(&lex, &tok) != 0) return 0;
        if (tok.type == TOK_EOF || tok.type == TOK_NEWLINE) return 0;
        /* There's a command after the label */
        cmd->type = DCL_NODE_COMMAND;
    }

    /* First meaningful token should be a WORD (the verb) */
    if (tok.type != TOK_WORD) {
        /* Not a word - unexpected token */
        return -1;
    }

    /* Check for symbol assignment: WORD followed by = or := or :== or == */
    dcl_token_t peek;
    if (dcl_lexer_peek(&lex, &peek) == 0) {
        if (peek.type == TOK_EQUALS || peek.type == TOK_COLON_EQUALS ||
            peek.type == TOK_COLON_COLON_EQUALS) {
            /* This is an assignment */
            cmd->type = DCL_NODE_ASSIGN;
            str_upcase_copy(cmd->verb, tok.value, sizeof(cmd->verb));

            dcl_token_t op;
            dcl_lexer_next(&lex, &op); /* consume the operator */

            /* Determine assignment type and store in label field for convenience:
             * "=" -> local, "==" -> global, ":=" -> local string, ":==" -> global string */
            if (op.type == TOK_COLON_COLON_EQUALS) {
                strncpy(cmd->label, ":==", sizeof(cmd->label) - 1);
            } else if (op.type == TOK_COLON_EQUALS) {
                strncpy(cmd->label, ":=", sizeof(cmd->label) - 1);
            } else if (strcmp(op.value, "==") == 0) {
                strncpy(cmd->label, "==", sizeof(cmd->label) - 1);
            } else {
                strncpy(cmd->label, "=", sizeof(cmd->label) - 1);
            }

            /* Collect the rest of the line as the value */
            collect_rest(&lex, cmd->rest, sizeof(cmd->rest));
            /* Also store in params[0] for convenience */
            strncpy(cmd->params[0], cmd->rest, sizeof(cmd->params[0]) - 1);
            cmd->param_count = 1;
            return 0;
        }
    }

    /* Regular command: verb [subcommand] [params] [/qualifiers] */
    str_upcase_copy(cmd->verb, tok.value, sizeof(cmd->verb));

    /* IF is handled specially: store the entire rest of line in cmd->rest
     * so the executor can parse condition and THEN clause without hitting
     * the DCL_MAX_PARAMS limit. */
    if (strcasecmp(cmd->verb, "IF") == 0) {
        collect_rest(&lex, cmd->rest, sizeof(cmd->rest));
        return 0;
    }

    /* Parse the rest of the tokens */
    while (1) {
        if (dcl_lexer_next(&lex, &tok) != 0) break;
        if (tok.type == TOK_EOF || tok.type == TOK_NEWLINE) break;

        switch (tok.type) {
        case TOK_QUALIFIER:
            parse_qualifier(&lex, &tok, cmd);
            break;

        case TOK_WORD:
        case TOK_STRING:
        case TOK_NUMBER:
            /* For certain verbs, first parameter is subcommand */
            if (cmd->param_count < DCL_MAX_PARAMS) {
                strncpy(cmd->params[cmd->param_count], tok.value,
                        sizeof(cmd->params[0]) - 1);
                cmd->params[cmd->param_count][sizeof(cmd->params[0]) - 1] = '\0';
                cmd->param_count++;
            }
            break;

        case TOK_PLUS:
            /* Append + to previous param or start new one */
            if (cmd->param_count > 0) {
                size_t len = strlen(cmd->params[cmd->param_count - 1]);
                if (len < sizeof(cmd->params[0]) - 2) {
                    cmd->params[cmd->param_count - 1][len] = '+';
                    cmd->params[cmd->param_count - 1][len + 1] = '\0';
                    /* Collect next word and append */
                    dcl_token_t next;
                    if (dcl_lexer_peek(&lex, &next) == 0 &&
                        (next.type == TOK_WORD || next.type == TOK_STRING)) {
                        dcl_lexer_next(&lex, &next);
                        size_t curlen = strlen(cmd->params[cmd->param_count - 1]);
                        strncat(cmd->params[cmd->param_count - 1], next.value,
                                sizeof(cmd->params[0]) - 1 - curlen);
                    }
                }
            }
            break;

        case TOK_COMMA:
            /* Comma-separated parameters (continue to next param) */
            break;

        case TOK_EQUALS:
            /* Could be part of qualifier value processing or something else */
            break;

        case TOK_PIPE:
            /* Store rest of line for PIPE processing */
            collect_rest(&lex, cmd->rest, sizeof(cmd->rest));
            break;

        default:
            /* For other tokens, collect rest of line */
            if (tok.type == TOK_DOT_AND || tok.type == TOK_DOT_OR ||
                tok.type == TOK_DOT_NOT || tok.type == TOK_EQ ||
                tok.type == TOK_NE || tok.type == TOK_LT ||
                tok.type == TOK_GT || tok.type == TOK_LE ||
                tok.type == TOK_GE || tok.type == TOK_EQS ||
                tok.type == TOK_NES || tok.type == TOK_LTS ||
                tok.type == TOK_GTS || tok.type == TOK_LES ||
                tok.type == TOK_GES) {
                /* Operator in expression context - add to params */
                if (cmd->param_count < DCL_MAX_PARAMS) {
                    strncpy(cmd->params[cmd->param_count], tok.value,
                            sizeof(cmd->params[0]) - 1);
                    cmd->param_count++;
                }
            }
            break;
        }
    }

    /* Store the whole rest-of-line for commands that need unparsed text */
    /* (This was already done partially; cmd->rest may have been set by PIPE) */

    return 0;
}

/*
 * Check if a qualifier is present in a parsed command.
 * Case-insensitive match. Returns 1 if found, 0 if not.
 */
int dcl_has_qualifier(const struct dcl_command *cmd, const char *name)
{
    if (!cmd || !name) return 0;
    for (int i = 0; i < cmd->qualifier_count; i++) {
        if (strcasecmp(cmd->qualifiers[i].name, name) == 0) {
            return cmd->qualifiers[i].negated ? 0 : 1;
        }
    }
    return 0;
}

/*
 * Get the value of a qualifier, or NULL if not present.
 */
const char *dcl_qualifier_value(const struct dcl_command *cmd, const char *name)
{
    if (!cmd || !name) return NULL;
    for (int i = 0; i < cmd->qualifier_count; i++) {
        if (strcasecmp(cmd->qualifiers[i].name, name) == 0) {
            if (cmd->qualifiers[i].value[0] != '\0')
                return cmd->qualifiers[i].value;
            return "";
        }
    }
    return NULL;
}
