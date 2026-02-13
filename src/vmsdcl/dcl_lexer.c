/*
 * dcl_lexer.c - Hand-written lexer for DCL command lines
 *
 * Tokenizes DCL input into a stream of tokens for the parser.
 * Handles VMS-specific syntax: qualifiers (/QUAL), dot operators
 * (.EQ., .AND., etc.), symbol substitution markers, quoted strings,
 * and line continuation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "dcl/lexer.h"

/*
 * Initialize the lexer state for a new input line.
 */
void dcl_lexer_init(dcl_lexer_t *lex, const char *input)
{
    lex->input = input;
    lex->pos = 0;
    lex->length = input ? strlen(input) : 0;
    lex->line = 1;
    lex->col = 1;
    lex->at_line_start = 1;
}

/*
 * Helper: peek at current character without advancing.
 */
static inline char lex_peek(dcl_lexer_t *lex)
{
    if (lex->pos >= lex->length) return '\0';
    return lex->input[lex->pos];
}

/*
 * Helper: peek at character at offset from current position.
 */
static inline char lex_peek_at(dcl_lexer_t *lex, size_t offset)
{
    size_t p = lex->pos + offset;
    if (p >= lex->length) return '\0';
    return lex->input[p];
}

/*
 * Helper: advance one character.
 */
static inline char lex_advance(dcl_lexer_t *lex)
{
    if (lex->pos >= lex->length) return '\0';
    char c = lex->input[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    return c;
}

/*
 * Helper: skip whitespace (but not newlines).
 */
static void skip_whitespace(dcl_lexer_t *lex)
{
    while (lex->pos < lex->length) {
        char c = lex->input[lex->pos];
        if (c == ' ' || c == '\t') {
            lex_advance(lex);
        } else {
            break;
        }
    }
}

/*
 * Helper: check if the rest of the line (after current pos) is only whitespace.
 */
static int rest_is_whitespace(dcl_lexer_t *lex)
{
    size_t p = lex->pos;
    while (p < lex->length) {
        char c = lex->input[p];
        if (c == '\n' || c == '\0') return 1;
        if (c != ' ' && c != '\t') return 0;
        p++;
    }
    return 1;
}

/*
 * Try to match a dot operator like .EQ., .EQS., .AND., etc.
 * Returns the token type if matched, or 0 if not.
 * Sets *len to the length of the matched operator.
 */
static dcl_token_type_t try_dot_operator(dcl_lexer_t *lex, size_t *len)
{
    /* Must start with '.' */
    if (lex_peek(lex) != '.') return 0;

    /* Build up the potential operator */
    const char *p = lex->input + lex->pos;
    size_t remaining = lex->length - lex->pos;

    /* Table of dot operators */
    static const struct {
        const char *text;
        size_t tlen;
        dcl_token_type_t type;
    } ops[] = {
        { ".EQS.", 5, TOK_EQS },
        { ".NES.", 5, TOK_NES },
        { ".LTS.", 5, TOK_LTS },
        { ".GTS.", 5, TOK_GTS },
        { ".LES.", 5, TOK_LES },
        { ".GES.", 5, TOK_GES },
        { ".AND.", 5, TOK_DOT_AND },
        { ".OR.",  4, TOK_DOT_OR },
        { ".NOT.", 5, TOK_DOT_NOT },
        { ".EQ.",  4, TOK_EQ },
        { ".NE.",  4, TOK_NE },
        { ".LT.",  4, TOK_LT },
        { ".GT.",  4, TOK_GT },
        { ".LE.",  4, TOK_LE },
        { ".GE.",  4, TOK_GE },
    };

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        if (remaining >= ops[i].tlen) {
            /* Case-insensitive compare */
            int match = 1;
            for (size_t j = 0; j < ops[i].tlen; j++) {
                if (toupper((unsigned char)p[j]) != ops[i].text[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                *len = ops[i].tlen;
                return ops[i].type;
            }
        }
    }

    return 0;
}

/*
 * Main tokenizer: get the next token from input.
 * Returns 0 on success, -1 on error.
 */
int dcl_lexer_next(dcl_lexer_t *lex, dcl_token_t *token)
{
    memset(token, 0, sizeof(*token));
    token->line = lex->line;
    token->col = lex->col;

    skip_whitespace(lex);

    /* End of input */
    if (lex->pos >= lex->length) {
        token->type = TOK_EOF;
        return 0;
    }

    char c = lex_peek(lex);
    token->line = lex->line;
    token->col = lex->col;

    /* Newline */
    if (c == '\n') {
        lex_advance(lex);
        token->type = TOK_NEWLINE;
        token->value[0] = '\n';
        token->value[1] = '\0';
        lex->at_line_start = 1;
        return 0;
    }

    /* Comment: ! or ; starts a comment to end of line */
    if (c == '!' || c == ';') {
        /* Consume the rest of the line */
        size_t vi = 0;
        while (lex->pos < lex->length && lex_peek(lex) != '\n') {
            if (vi < sizeof(token->value) - 1) {
                token->value[vi++] = lex_advance(lex);
            } else {
                lex_advance(lex);
            }
        }
        token->value[vi] = '\0';
        token->type = TOK_EOF; /* Treat comment as end of meaningful input */
        return 0;
    }

    /* Quoted string: "..." with "" for literal quote */
    if (c == '"') {
        lex_advance(lex); /* consume opening quote */
        size_t vi = 0;
        while (lex->pos < lex->length) {
            char ch = lex_peek(lex);
            if (ch == '"') {
                lex_advance(lex);
                /* Check for doubled quote */
                if (lex_peek(lex) == '"') {
                    if (vi < sizeof(token->value) - 1)
                        token->value[vi++] = '"';
                    lex_advance(lex);
                } else {
                    break; /* End of string */
                }
            } else if (ch == '\n' || ch == '\0') {
                break; /* Unterminated string */
            } else {
                if (vi < sizeof(token->value) - 1)
                    token->value[vi++] = ch;
                lex_advance(lex);
            }
        }
        token->value[vi] = '\0';
        token->type = TOK_STRING;
        lex->at_line_start = 0;
        return 0;
    }

    /* Qualifier: /NAME or /NONAME */
    if (c == '/') {
        lex_advance(lex); /* consume / */
        size_t vi = 0;
        /* Read qualifier name (alpha, digits, underscore, $) */
        while (lex->pos < lex->length) {
            char ch = lex_peek(lex);
            if (isalnum((unsigned char)ch) || ch == '_' || ch == '$') {
                if (vi < sizeof(token->value) - 1)
                    token->value[vi++] = (char)toupper((unsigned char)ch);
                lex_advance(lex);
            } else {
                break;
            }
        }
        token->value[vi] = '\0';
        token->type = TOK_QUALIFIER;
        lex->at_line_start = 0;
        return 0;
    }

    /* Check for := and :== assignment operators.
     * We need to look ahead: if we see ':' followed by '=' or '==',
     * treat it as an assignment operator. Otherwise it's part of a word. */
    if (c == ':' && lex_peek_at(lex, 1) == '=') {
        lex_advance(lex); /* consume : */
        lex_advance(lex); /* consume = */
        if (lex_peek(lex) == '=') {
            lex_advance(lex); /* consume second = */
            token->type = TOK_COLON_COLON_EQUALS;
            strcpy(token->value, ":==");
        } else {
            token->type = TOK_COLON_EQUALS;
            strcpy(token->value, ":=");
        }
        lex->at_line_start = 0;
        return 0;
    }

    /* Equals sign */
    if (c == '=') {
        lex_advance(lex);
        /* Check for == (global assignment) */
        if (lex_peek(lex) == '=') {
            lex_advance(lex);
            token->type = TOK_EQUALS; /* We'll use context to differentiate */
            strcpy(token->value, "==");
        } else {
            token->type = TOK_EQUALS;
            strcpy(token->value, "=");
        }
        lex->at_line_start = 0;
        return 0;
    }

    /* At sign (@) - procedure invocation */
    if (c == '@') {
        lex_advance(lex);
        token->type = TOK_AT;
        token->value[0] = '@';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }

    /* Comma */
    if (c == ',') {
        lex_advance(lex);
        token->type = TOK_COMMA;
        token->value[0] = ',';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }

    /* Parentheses */
    if (c == '(') {
        lex_advance(lex);
        token->type = TOK_LPAREN;
        token->value[0] = '(';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }
    if (c == ')') {
        lex_advance(lex);
        token->type = TOK_RPAREN;
        token->value[0] = ')';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }

    /* Plus */
    if (c == '+') {
        lex_advance(lex);
        token->type = TOK_PLUS;
        token->value[0] = '+';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }

    /* Minus: if at end of line, it's continuation; otherwise minus */
    if (c == '-') {
        lex_advance(lex);
        if (rest_is_whitespace(lex)) {
            token->type = TOK_CONTINUATION;
            token->value[0] = '-';
            token->value[1] = '\0';
        } else {
            token->type = TOK_MINUS;
            token->value[0] = '-';
            token->value[1] = '\0';
        }
        lex->at_line_start = 0;
        return 0;
    }

    /* Pipe */
    if (c == '|') {
        lex_advance(lex);
        token->type = TOK_PIPE;
        token->value[0] = '|';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }

    /* Apostrophe - symbol substitution marker */
    if (c == '\'') {
        lex_advance(lex);
        token->type = TOK_APOSTROPHE;
        token->value[0] = '\'';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }

    /* Ampersand - symbol substitution marker */
    if (c == '&') {
        lex_advance(lex);
        token->type = TOK_AMPERSAND;
        token->value[0] = '&';
        token->value[1] = '\0';
        lex->at_line_start = 0;
        return 0;
    }

    /* Dot operators: .EQ., .NES., .AND., etc. */
    if (c == '.') {
        size_t oplen = 0;
        dcl_token_type_t optype = try_dot_operator(lex, &oplen);
        if (optype != 0) {
            size_t vi = 0;
            for (size_t i = 0; i < oplen; i++) {
                if (vi < sizeof(token->value) - 1)
                    token->value[vi++] = (char)toupper((unsigned char)lex_advance(lex));
            }
            token->value[vi] = '\0';
            token->type = optype;
            lex->at_line_start = 0;
            return 0;
        }
        /* Not a dot operator; fall through to word scanning */
    }

    /* Word: alphanumeric, $, _, ., :, *, %, ~ (for filespecs) */
    if (isalnum((unsigned char)c) || c == '_' || c == '$' || c == '.'
        || c == '*' || c == '%' || c == '~' || c == '[' || c == ']') {
        size_t vi = 0;
        int had_colon = 0;
        while (lex->pos < lex->length) {
            char ch = lex_peek(lex);
            if (isalnum((unsigned char)ch) || ch == '_' || ch == '$'
                || ch == '.' || ch == '*' || ch == '%' || ch == '~'
                || ch == '#' || ch == '-' || ch == '['
                || ch == ']') {
                if (vi < sizeof(token->value) - 1)
                    token->value[vi++] = ch;
                lex_advance(lex);
            } else if (ch == ':') {
                /* Colon could be: part of label (at end), device name,
                 * or start of := assignment */
                if (lex_peek_at(lex, 1) == '=') {
                    /* This is := assignment - stop word here */
                    break;
                } else if (lex_peek_at(lex, 1) == ':') {
                    /* Could be :: (DECnet node) or ::= */
                    if (lex_peek_at(lex, 2) == '=') {
                        break; /* :== assignment */
                    }
                    /* DECnet :: - include in word */
                    if (vi < sizeof(token->value) - 1)
                        token->value[vi++] = ch;
                    lex_advance(lex);
                    if (vi < sizeof(token->value) - 1)
                        token->value[vi++] = lex_advance(lex);
                } else {
                    /* Single colon: include it (could be device: or label:) */
                    if (vi < sizeof(token->value) - 1)
                        token->value[vi++] = ch;
                    lex_advance(lex);
                    had_colon = 1;
                    /* Check if this is end of word (label or device) */
                    char next = lex_peek(lex);
                    if (next == ' ' || next == '\t' || next == '\0' ||
                        next == '\n' || next == '!') {
                        /* Ends with colon and followed by space/EOL - could be label */
                        break;
                    }
                    /* If followed by '[' it's a device:[dir] - continue */
                }
            } else {
                break;
            }
        }
        token->value[vi] = '\0';

        /* Check if it's a label (word ending with :, at start of line) */
        if (had_colon && vi > 1 && token->value[vi - 1] == ':'
            && lex->at_line_start) {
            token->type = TOK_LABEL;
            token->value[vi - 1] = '\0'; /* Remove trailing colon */
        } else {
            /* Check if it's all digits -> NUMBER */
            int all_digits = 1;
            for (size_t i = 0; i < vi; i++) {
                if (!isdigit((unsigned char)token->value[i])) {
                    all_digits = 0;
                    break;
                }
            }
            if (all_digits && vi > 0) {
                token->type = TOK_NUMBER;
            } else {
                token->type = TOK_WORD;
            }
        }
        lex->at_line_start = 0;
        return 0;
    }

    /* Unknown character - consume and return as WORD */
    lex_advance(lex);
    token->type = TOK_WORD;
    token->value[0] = c;
    token->value[1] = '\0';
    lex->at_line_start = 0;
    return 0;
}

/*
 * Peek at the next token without consuming it.
 * Saves and restores lexer state.
 */
int dcl_lexer_peek(dcl_lexer_t *lex, dcl_token_t *token)
{
    /* Save state */
    size_t saved_pos = lex->pos;
    int saved_line = lex->line;
    int saved_col = lex->col;
    int saved_start = lex->at_line_start;

    int result = dcl_lexer_next(lex, token);

    /* Restore state */
    lex->pos = saved_pos;
    lex->line = saved_line;
    lex->col = saved_col;
    lex->at_line_start = saved_start;

    return result;
}
