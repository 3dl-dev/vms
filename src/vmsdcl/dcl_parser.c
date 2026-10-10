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
#include "dcl/cdu.h"
#include "str_util.h"
#include "ssdef.h"

/*
 * Declared locally rather than via "dcl/dcl_cmd.h": that header also declares
 * prototypes using mode_t / struct dcl_context, which are not in scope in this
 * TU under the VMS-native freestanding+musl compile (no <sys/types.h>), and
 * pulling it in broke the DCL.EXE self-host/link graph. We need only the error
 * emitter here. Signature matches dcl_cmd.h and dcl_io.c's definition.
 */
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);

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

    /* Capture the raw, unparsed argument tail (everything after the verb) into
     * cmd->rest WITHOUT advancing the lexer. This is what a foreign-command
     * dispatch hands to the activated image as argv the OpenVMS way -- real DCL
     * passes the whole command tail to the image (LIB$GET_FOREIGN) and the
     * image's own CRTL splits it into argc/argv, so the P1-P8 (DCL_MAX_PARAMS)
     * cap that the token loop below enforces does not apply to foreign commands.
     * The token loop still runs afterward for builtins/qualifiers; cmd->rest is
     * only consumed by dcl_exec_foreign_command. It is kept in its OWN field
     * (raw_tail), NOT cmd->rest, because cmd->rest has existing consumers that
     * assume it is empty for an ordinary command (e.g. cmd_spawn appends it as a
     * post-pipe remnant -- populating rest here made `SPAWN SHOW PROCESS` spawn
     * "SHOW PROCESS SHOW PROCESS"). vms-615. */
    {
        const char *tail = lex.input + lex.pos;
        while (*tail == ' ' || *tail == '\t') tail++;
        strncpy(cmd->raw_tail, tail, sizeof(cmd->raw_tail) - 1);
        cmd->raw_tail[sizeof(cmd->raw_tail) - 1] = '\0';
        size_t rl = strlen(cmd->raw_tail);
        while (rl > 0 && (cmd->raw_tail[rl - 1] == ' ' || cmd->raw_tail[rl - 1] == '\t' ||
                          cmd->raw_tail[rl - 1] == '\n'))
            cmd->raw_tail[--rl] = '\0';
    }

    /*
     * SPAWN [/qualifier...] [command-string] (rd vms-f37). SPAWN's qualifiers
     * are the ones written straight after the verb; from the first parameter
     * on, the rest of the line is ONE command string for the subprocess, its
     * own qualifiers included (OpenVMS DCL Dictionary, SPAWN). Parsing on
     * gave "SPAWN SHOW PROCESS/PRIVILEGES" a %DCL-W-IVQUAL \PRIVILEGES\ as if
     * the qualifier were SPAWN's. So only SPAWN's leading qualifiers are left
     * to the token loop, and the command string becomes params[0] verbatim.
     */
    char spawn_cmd[DCL_MAX_VALUE];
    int  spawn_has_cmd = 0;
    {
        size_t vl = strlen(cmd->verb);
        if (vl >= 2 && vl <= 5 && strncasecmp(cmd->verb, "SPAWN", vl) == 0) {
            const char *base = lex.input;
            size_t i = lex.pos;
            for (;;) {
                while (i < lex.length && (base[i] == ' ' || base[i] == '\t'))
                    i++;
                if (i >= lex.length || base[i] != '/')
                    break;
                /* one qualifier: up to the next blank or '/', a quoted or
                 * parenthesised value taken whole */
                int depth = 0, inq = 0;
                i++;
                while (i < lex.length) {
                    char ch = base[i];
                    if (inq) { if (ch == '"') inq = 0; }
                    else if (ch == '"') inq = 1;
                    else if (ch == '(') depth++;
                    else if (ch == ')') { if (depth) depth--; }
                    else if (!depth && (ch == ' ' || ch == '\t' || ch == '/'))
                        break;
                    i++;
                }
            }
            if (i < lex.length) {
                size_t n = lex.length - i;
                if (n >= sizeof(spawn_cmd)) n = sizeof(spawn_cmd) - 1;
                memcpy(spawn_cmd, base + i, n);
                spawn_cmd[n] = '\0';
                while (n > 0 && (spawn_cmd[n - 1] == ' ' || spawn_cmd[n - 1] == '\t' ||
                                 spawn_cmd[n - 1] == '\n'))
                    spawn_cmd[--n] = '\0';
                spawn_has_cmd = n > 0;
                lex.length = i;          /* the token loop sees SPAWN's qualifiers only */
            }
        }
    }

    /* Parse the rest of the tokens */
    int last_was_param = 0;       /* previous token was a WORD/STRING/NUMBER param */
    int last_param_quoted = 0;    /* ...and that param came from a bare STRING     */
    while (1) {
        size_t before = lex.pos;
        if (dcl_lexer_next(&lex, &tok) != 0) break;
        if (tok.type == TOK_EOF || tok.type == TOK_NEWLINE) break;
        /* ADJACENT tokens with no blank between them are ONE DCL parameter
         * (rd vms-a8a lab: `COPY VAX1"SYSTEM pw"::T1.TXT X` was split into three
         * parameters at the quotes). DCL keeps an embedded quoted span inside
         * the parameter WITH its quotes and its case; only a parameter that is
         * entirely one quoted string loses them (the unchanged path below). */
        int adjacent = before < lex.length && lex.input[before] != ' ' &&
                       lex.input[before] != '\t';
        if (adjacent && last_was_param && cmd->param_count > 0 &&
            (tok.type == TOK_WORD || tok.type == TOK_STRING || tok.type == TOK_NUMBER)) {
            char *dst = cmd->params[cmd->param_count - 1];
            char merged[sizeof(cmd->params[0])];
            size_t m = 0, cap = sizeof merged - 1;
            const char *src = dst;
            if (last_param_quoted && m < cap) merged[m++] = '"';
            for (; *src && m < cap; src++) {
                if (last_param_quoted && *src == '"' && m + 1 < cap) merged[m++] = '"';
                merged[m++] = *src;
            }
            if (last_param_quoted && m < cap) merged[m++] = '"';
            if (tok.type == TOK_STRING && m < cap) merged[m++] = '"';
            for (src = tok.value; *src && m < cap; src++) {
                if (tok.type == TOK_STRING && *src == '"' && m + 1 < cap) merged[m++] = '"';
                merged[m++] = *src;
            }
            if (tok.type == TOK_STRING && m < cap) merged[m++] = '"';
            merged[m] = '\0';
            memcpy(dst, merged, m + 1);
            last_param_quoted = 0;
            continue;
        }
        last_was_param = (tok.type == TOK_WORD || tok.type == TOK_STRING ||
                          tok.type == TOK_NUMBER);
        last_param_quoted = (tok.type == TOK_STRING);

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

        case TOK_AT:
            /*
             * '@' IN A PARAMETER POSITION (rd vms-a18).
             *
             * '@' is honoured as a VERB only at the start of a line (the
             * TOK_AT arm near the top of this function). Everywhere else it is
             * the first character of a COMMAND STRING this command carries to
             * somebody else -- `SPAWN @PROC.COM`, `SPAWN/NOWAIT
             * @SYS$LOGIN:X.COM` -- and the default arm below silently DROPPED
             * it, because TOK_AT is not one of the param token types. cmd_spawn
             * then joined the surviving parameters, so the SPAWNed DCL was
             * handed the bare filespec as its VERB and answered %DCL-E-IVVERB.
             *
             * Keep it, glued to the word that follows, the way DCL's own
             * command tail carries it. The following word is consumed here
             * (the TOK_PLUS arm's idiom) whether or not a blank separates it,
             * because OpenVMS accepts `@ FILE.COM` as well as `@FILE.COM`. A
             * trailing '@' with no word after it stays its own parameter, so
             * the consumer reports its own error instead of acting on a command
             * string that quietly lost a character.
             */
            if (cmd->param_count < DCL_MAX_PARAMS) {
                char *dst = cmd->params[cmd->param_count];
                dcl_token_t next;
                dst[0] = '@';
                dst[1] = '\0';
                if (dcl_lexer_peek(&lex, &next) == 0 &&
                    (next.type == TOK_WORD || next.type == TOK_STRING ||
                     next.type == TOK_NUMBER)) {
                    dcl_lexer_next(&lex, &next);
                    strncat(dst, next.value, sizeof(cmd->params[0]) - 2);
                }
                cmd->param_count++;
                last_was_param = 1;
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

    if (spawn_has_cmd) {                 /* SPAWN's command string (rd vms-f37) */
        strncpy(cmd->params[0], spawn_cmd, sizeof(cmd->params[0]) - 1);
        cmd->params[0][sizeof(cmd->params[0]) - 1] = '\0';
        cmd->param_count = 1;
    }

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
 * Resolve a parsed qualifier name against a verb's declared qualifier table.
 * Supports VMS-style qualifier abbreviation: an exact (case-insensitive) match
 * wins; otherwise a unique case-insensitive prefix match resolves. An ambiguous
 * prefix (two or more table entries share it) does NOT resolve -- the caller
 * treats that as unknown (%DCL-W-IVQUAL), never silently picking one. Returns
 * the matching def, or NULL.
 */
static const struct dcl_qual_def *dcl_qual_lookup(
        const struct dcl_qual_def *table, const char *name)
{
    if (!table || !name || !name[0]) return NULL;

    const struct dcl_qual_def *prefix_hit = NULL;
    int prefix_count = 0;
    size_t nlen = strlen(name);

    for (const struct dcl_qual_def *d = table; d->name; d++) {
        if (strcasecmp(d->name, name) == 0)
            return d;                    /* exact match wins outright */
        if (strncasecmp(d->name, name, nlen) == 0) {
            prefix_hit = d;
            prefix_count++;
        }
    }
    return (prefix_count == 1) ? prefix_hit : NULL;
}

/*
 * Case-insensitive keyword-set membership with the same abbreviation rule as
 * qualifier names (exact, else unique prefix). Returns 1 if 'value' names a
 * legal keyword in the NULL-terminated set, 0 otherwise.
 */
static int dcl_keyword_ok(const char *const *keywords, const char *value)
{
    if (!keywords || !value) return 0;
    const char *prefix_hit = NULL;
    int prefix_count = 0;
    size_t vlen = strlen(value);

    for (const char *const *k = keywords; *k; k++) {
        if (strcasecmp(*k, value) == 0) return 1;
        if (strncasecmp(*k, value, vlen) == 0) {
            prefix_hit = *k;
            prefix_count++;
        }
    }
    (void)prefix_hit;
    return (prefix_count == 1) ? 1 : 0;
}

/*
 * Validate a parsed command's qualifiers against the verb's declared table.
 * See dcl/cdu.h for the contract. This is the Phase 1 keystone
 * (docs/design-dcl-fidelity.md sec 4): once a verb declares a qualifier table,
 * %DCL-W-IVQUAL (unknown qualifier) and %DCL-W-IVKEYW (bad keyword value)
 * become structurally reachable for it, and abbreviated qualifier names are
 * canonicalised so the existing dcl_has_qualifier()/dcl_qualifier_value()
 * handler reads keep matching.
 *
 * Error text is grounded to the reference lab / DCL Dictionary, matching the
 * Phase 0 canary in cmd_set_terminal() (dcl_cmd_set.c): VAX1 capture,
 * "unrecognized qualifier - check validity, spelling, and placement".
 */
int dcl_validate_qualifiers(const struct dcl_verb *verb,
                            struct dcl_command *cmd)
{
    if (!verb || !verb->quals || !cmd) return SS$_NORMAL; /* not retrofit */

    for (int i = 0; i < cmd->qualifier_count; i++) {
        struct dcl_qualifier *q = &cmd->qualifiers[i];
        int negated = q->negated;

        const struct dcl_qual_def *def = dcl_qual_lookup(verb->quals, q->name);

        /* The parser's /NOxxx split is a heuristic: for a qualifier whose real
         * name begins with "NO", the "NO" was wrongly stripped. If the stripped
         * name did not resolve but "NO"+name does, it is a literal qualifier,
         * not a negation -- undo the split. */
        if (!def && negated) {
            char full[80];
            snprintf(full, sizeof(full), "NO%s", q->name);
            const struct dcl_qual_def *d2 = dcl_qual_lookup(verb->quals, full);
            if (d2) {
                def = d2;
                negated = 0;
                q->negated = 0;
                str_upcase_copy(q->name, full, sizeof(q->name));
            }
        }

        if (!def) {
            /* Unknown qualifier -> authentic %DCL-W-IVQUAL. Show the name as
             * the user typed it (restore the NO prefix if it was stripped). */
            char shown[80];
            if (negated)
                snprintf(shown, sizeof(shown), "NO%s", q->name);
            else
                snprintf(shown, sizeof(shown), "%s", q->name);
            dcl_error("DCL", 0, "IVQUAL",
                      "unrecognized qualifier - check validity, spelling, "
                      "and placement - \\%s\\", shown);
            return SS$_IVQUAL;
        }

        /* /NO form on a qualifier that is not negatable -> IVQUAL. */
        if (negated && !(def->qflags & CDU_Q_NEGATABLE)) {
            dcl_error("DCL", 0, "IVQUAL",
                      "unrecognized qualifier - check validity, spelling, "
                      "and placement - \\NO%s\\", def->name);
            return SS$_IVQUAL;
        }

        /* Canonicalise an abbreviated name to the declared full name so
         * handler reads by full name still match. */
        str_upcase_copy(q->name, def->name, sizeof(q->name));

        /* Keyword value-type: a supplied value must be a legal keyword. The
         * negated form never carries a value, so only check when present. */
        if (!negated && def->vtype == CDU_VT_KEYWORD && q->value[0] != '\0') {
            if (!dcl_keyword_ok(def->keywords, q->value)) {
                dcl_error("DCL", 0, "IVKEYW",
                          "unrecognized keyword - check validity and "
                          "spelling - \\%s\\", q->value);
                return SS$_IVKEYW;
            }
        }
    }
    return SS$_NORMAL;
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
