/*
 * dcl_exec.c - DCL Command Execution Dispatch
 *
 * Dispatches parsed DCL commands to their handler functions.
 * Handles symbol assignment, procedure invocation, IF/THEN/ELSE
 * flow control, and command verb lookup with minimum-uniqueness.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/cdu.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"

/* External functions */
extern int dcl_execute_script(const char *filename, int argc, char **argv);
extern void dcl_error(const char *facility, int severity, const char *ident,
                      const char *fmt, ...);
extern int dcl_eval_lexical(struct dcl_context *ctx, const char *expr,
                            char *result, size_t result_size);
extern int dcl_find_label(FILE *fp, const char *label);
extern int dcl_execute_line(const char *line);

/* -------------------------------------------------------------------------
 * DCL Expression Evaluator
 *
 * Handles both integer arithmetic (+ - * /) and string operations
 * (+ for concatenation, - for substring removal) in symbol assignments.
 *
 * Grammar (integer mode):
 *   expr   = term (('+' | '-') term)*
 *   term   = factor (('*' | '/') factor)*
 *   factor = '(' expr ')' | integer_literal | string_literal | symbol_ref
 *
 * Type detection: if either operand is a string, use string semantics.
 * -------------------------------------------------------------------------
 */

/*
 * Expression value — either an integer or a string.
 */
typedef struct {
    int     is_string;  /* 1 = string value, 0 = integer value */
    long    ival;
    char    sval[DCL_MAX_VALUE];
} expr_val_t;

/*
 * Parser state for recursive descent.
 */
typedef struct {
    const char *input;
    size_t      pos;
    size_t      len;
    struct dcl_context *ctx;
} expr_parser_t;

static void ep_skip_ws(expr_parser_t *ep)
{
    while (ep->pos < ep->len &&
           (ep->input[ep->pos] == ' ' || ep->input[ep->pos] == '\t'))
        ep->pos++;
}

static char ep_peek(expr_parser_t *ep)
{
    if (ep->pos >= ep->len) return '\0';
    return ep->input[ep->pos];
}

static char ep_consume(expr_parser_t *ep)
{
    if (ep->pos >= ep->len) return '\0';
    return ep->input[ep->pos++];
}

/* Forward declaration */
static expr_val_t parse_expr(expr_parser_t *ep);

/*
 * Make integer value.
 */
static expr_val_t make_int(long v)
{
    expr_val_t r;
    r.is_string = 0;
    r.ival = v;
    r.sval[0] = '\0';
    return r;
}

/*
 * Make string value.
 */
static expr_val_t make_str(const char *s)
{
    expr_val_t r;
    r.is_string = 1;
    r.ival = 0;
    strncpy(r.sval, s, sizeof(r.sval) - 1);
    r.sval[sizeof(r.sval) - 1] = '\0';
    return r;
}

/*
 * Coerce value to string representation.
 */
static const char *val_to_str(expr_val_t *v, char *buf, size_t bufsz)
{
    if (v->is_string) return v->sval;
    snprintf(buf, bufsz, "%ld", v->ival);
    return buf;
}

/*
 * Parse a primary (atom): number, quoted string, symbol reference, or
 * parenthesised sub-expression.
 */
static expr_val_t parse_primary(expr_parser_t *ep)
{
    ep_skip_ws(ep);
    char c = ep_peek(ep);

    /* Parenthesised expression */
    if (c == '(') {
        ep_consume(ep); /* ( */
        expr_val_t v = parse_expr(ep);
        ep_skip_ws(ep);
        if (ep_peek(ep) == ')') ep_consume(ep);
        return v;
    }

    /* Quoted string literal */
    if (c == '"') {
        ep_consume(ep); /* opening " */
        char buf[DCL_MAX_VALUE];
        size_t bi = 0;
        while (ep->pos < ep->len) {
            char ch = ep->input[ep->pos];
            if (ch == '"') {
                ep->pos++;
                /* Doubled quote = literal " */
                if (ep->pos < ep->len && ep->input[ep->pos] == '"') {
                    if (bi < sizeof(buf) - 1) buf[bi++] = '"';
                    ep->pos++;
                } else {
                    break;
                }
            } else {
                if (bi < sizeof(buf) - 1) buf[bi++] = ch;
                ep->pos++;
            }
        }
        buf[bi] = '\0';
        return make_str(buf);
    }

    /* Apostrophe symbol substitution: 'SYMNAME' */
    if (c == '\'') {
        ep_consume(ep); /* ' */
        char symname[256];
        size_t si = 0;
        while (ep->pos < ep->len && si < sizeof(symname) - 1) {
            char ch = ep->input[ep->pos];
            if (ch == '\'') { ep->pos++; break; }
            if (isalnum((unsigned char)ch) || ch == '_' || ch == '$') {
                symname[si++] = ch;
                ep->pos++;
            } else break;
        }
        symname[si] = '\0';
        const char *val = dcl_sym_get(symname);
        if (!val) return make_int(0);
        /* Try integer */
        char *endp;
        long iv = strtol(val, &endp, 0);
        if (*endp == '\0' && val[0] != '\0') return make_int(iv);
        return make_str(val);
    }

    /* Ampersand symbol substitution: &SYMNAME */
    if (c == '&') {
        ep_consume(ep); /* & */
        char symname[256];
        size_t si = 0;
        while (ep->pos < ep->len && si < sizeof(symname) - 1) {
            char ch = ep->input[ep->pos];
            if (isalnum((unsigned char)ch) || ch == '_' || ch == '$') {
                symname[si++] = ch;
                ep->pos++;
            } else break;
        }
        symname[si] = '\0';
        const char *val = dcl_sym_get(symname);
        if (!val) return make_int(0);
        char *endp;
        long iv = strtol(val, &endp, 0);
        if (*endp == '\0' && val[0] != '\0') return make_int(iv);
        return make_str(val);
    }

    /* Lexical function F$xxx(...) */
    if ((c == 'F' || c == 'f') &&
        ep->pos + 1 < ep->len &&
        (ep->input[ep->pos + 1] == '$')) {
        /* Collect to end of function call (balance parens) */
        char buf[DCL_MAX_VALUE];
        size_t bi = 0;
        int depth = 0;
        while (ep->pos < ep->len) {
            char ch = ep->input[ep->pos];
            if (ch == '(') depth++;
            else if (ch == ')') {
                if (depth == 0) break;
                depth--;
            } else if (depth == 0 &&
                       (ch == ' ' || ch == '+' || ch == '-' ||
                        ch == '*' || ch == '/')) {
                if (ch != ' ') break;
            }
            if (bi < sizeof(buf) - 1) buf[bi++] = ch;
            ep->pos++;
        }
        buf[bi] = '\0';
        char result[DCL_MAX_VALUE];
        result[0] = '\0';
        if (ep->ctx)
            dcl_eval_lexical(ep->ctx, buf, result, sizeof(result));
        char *endp;
        long iv = strtol(result, &endp, 0);
        if (*endp == '\0' && result[0] != '\0') return make_int(iv);
        return make_str(result);
    }

    /* Numeric literal (possibly hex with 0x prefix) */
    if (isdigit((unsigned char)c) ||
        (c == '-' && ep->pos + 1 < ep->len &&
         isdigit((unsigned char)ep->input[ep->pos + 1]))) {
        char buf[64];
        size_t bi = 0;
        if (c == '-') { buf[bi++] = ep_consume(ep); }
        /* Check for hex */
        if (ep_peek(ep) == '0' &&
            ep->pos + 1 < ep->len &&
            (ep->input[ep->pos + 1] == 'x' || ep->input[ep->pos + 1] == 'X')) {
            buf[bi++] = ep_consume(ep); /* 0 */
            buf[bi++] = ep_consume(ep); /* x */
        }
        while (ep->pos < ep->len && bi < sizeof(buf) - 1) {
            char ch = ep->input[ep->pos];
            if (isxdigit((unsigned char)ch)) {
                buf[bi++] = ch;
                ep->pos++;
            } else break;
        }
        buf[bi] = '\0';
        char *endp;
        long iv = strtol(buf, &endp, 0);
        return make_int(iv);
    }

    /* Unquoted word — treat as symbol reference or string literal */
    if (isalpha((unsigned char)c) || c == '_' || c == '$') {
        char word[256];
        size_t wi = 0;
        while (ep->pos < ep->len && wi < sizeof(word) - 1) {
            char ch = ep->input[ep->pos];
            if (isalnum((unsigned char)ch) || ch == '_' || ch == '$') {
                word[wi++] = ch;
                ep->pos++;
            } else break;
        }
        word[wi] = '\0';

        /* Look up as symbol first */
        const char *val = dcl_sym_get(word);
        if (val) {
            char *endp;
            long iv = strtol(val, &endp, 0);
            if (*endp == '\0' && val[0] != '\0') return make_int(iv);
            return make_str(val);
        }
        /* Not a symbol - treat as string literal */
        return make_str(word);
    }

    /* Anything else: return 0/empty */
    return make_int(0);
}

/*
 * Parse multiplicative expression: factor (* | /) factor ...
 */
static expr_val_t parse_term(expr_parser_t *ep)
{
    expr_val_t left = parse_primary(ep);

    while (1) {
        ep_skip_ws(ep);
        char op = ep_peek(ep);
        if (op != '*' && op != '/') break;
        ep_consume(ep);
        ep_skip_ws(ep);
        expr_val_t right = parse_primary(ep);

        /* Integer-only operations */
        if (left.is_string || right.is_string) {
            /* Type error — leave left unchanged */
            break;
        }
        if (op == '*') {
            left.ival *= right.ival;
        } else {
            if (right.ival == 0) {
                /* Division by zero: VMS yields 0 */
                left.ival = 0;
            } else {
                left.ival /= right.ival;
            }
        }
    }
    return left;
}

/*
 * Parse additive expression: term (('+' | '-') term) ...
 *
 * When both operands are integers: arithmetic.
 * When either operand is a string:
 *   '+' = concatenation
 *   '-' = remove first occurrence of right from left
 */
static expr_val_t parse_expr(expr_parser_t *ep)
{
    expr_val_t left = parse_term(ep);

    while (1) {
        ep_skip_ws(ep);
        char op = ep_peek(ep);
        if (op != '+' && op != '-') break;
        ep_consume(ep);
        ep_skip_ws(ep);
        expr_val_t right = parse_term(ep);

        if (!left.is_string && !right.is_string) {
            /* Integer arithmetic */
            if (op == '+') left.ival += right.ival;
            else           left.ival -= right.ival;
        } else {
            /* String semantics */
            char lbuf[64], rbuf[64];
            const char *ls = val_to_str(&left,  lbuf, sizeof(lbuf));
            const char *rs = val_to_str(&right, rbuf, sizeof(rbuf));

            if (op == '+') {
                /* Concatenate */
                char result[DCL_MAX_VALUE];
                size_t ll = strlen(ls);
                size_t rl = strlen(rs);
                if (ll + rl >= sizeof(result))
                    rl = sizeof(result) - ll - 1;
                memcpy(result, ls, ll);
                memcpy(result + ll, rs, rl);
                result[ll + rl] = '\0';
                left = make_str(result);
            } else {
                /* Remove first occurrence of rs from ls */
                char result[DCL_MAX_VALUE];
                const char *found = strstr(ls, rs);
                if (found) {
                    size_t prefix = (size_t)(found - ls);
                    size_t rlen = strlen(rs);
                    size_t suffix = strlen(found + rlen);
                    if (prefix + suffix >= sizeof(result))
                        suffix = sizeof(result) - prefix - 1;
                    memcpy(result, ls, prefix);
                    memcpy(result + prefix, found + rlen, suffix);
                    result[prefix + suffix] = '\0';
                } else {
                    strncpy(result, ls, sizeof(result) - 1);
                    result[sizeof(result) - 1] = '\0';
                }
                left = make_str(result);
            }
        }
    }
    return left;
}

/*
 * Evaluate a DCL expression string.
 * Performs symbol substitution then arithmetic/string evaluation.
 * Returns the result in *out_val.
 */
static void eval_expr(struct dcl_context *ctx, const char *expr,
                      expr_val_t *out_val)
{
    /* First pass: symbol substitution */
    char subst[DCL_MAX_VALUE];
    dcl_sym_substitute(expr, subst, sizeof(subst));

    expr_parser_t ep;
    ep.input = subst;
    ep.pos   = 0;
    ep.len   = strlen(subst);
    ep.ctx   = ctx;

    *out_val = parse_expr(&ep);
}

/*
 * Check whether a string contains any arithmetic operator outside of
 * quotes or parentheses. Used to decide whether to evaluate or assign
 * as a plain string.
 */
static int has_arith_op(const char *s)
{
    int depth = 0;
    int in_quote = 0;
    for (const char *p = s; *p; p++) {
        if (in_quote) {
            if (*p == '"') {
                /* Doubled quote: peek ahead */
                if (*(p + 1) == '"') p++;
                else in_quote = 0;
            }
            continue;
        }
        if (*p == '"') { in_quote = 1; continue; }
        if (*p == '(') { depth++; continue; }
        if (*p == ')') { depth--; continue; }
        if (depth == 0 && (*p == '+' || *p == '-' || *p == '*' || *p == '/'))
            return 1;
    }
    return 0;
}

/*
 * -------------------------------------------------------------------------
 * Condition evaluator
 *
 * Supports full boolean expressions:
 *   .NOT. expr
 *   expr .AND. expr
 *   expr .OR. expr
 *   expr .EQ.|.NE.|.LT.|.GT.|.LE.|.GE. expr     (integer)
 *   expr .EQS.|.NES.|.LTS.|.GTS.|.LES.|.GES. expr (string)
 *   (expr)
 *
 * Operator precedence (low to high):
 *   .OR.
 *   .AND.
 *   .NOT.
 *   comparison operators (.EQ., .GT., etc.)
 *   primary (value)
 * -------------------------------------------------------------------------
 */

/* Condition parser state */
typedef struct {
    const char *input;
    size_t      pos;
    size_t      len;
    struct dcl_context *cctx;
} cond_parser_t;

static void cp_skip_ws(cond_parser_t *cp)
{
    while (cp->pos < cp->len &&
           (cp->input[cp->pos] == ' ' || cp->input[cp->pos] == '\t'))
        cp->pos++;
}

/*
 * Match a dot operator at current position (case-insensitive).
 * Returns length of operator if matched, 0 otherwise.
 */
static size_t cp_match_op(cond_parser_t *cp, const char *op)
{
    size_t oplen = strlen(op);
    if (cp->pos + oplen > cp->len) return 0;
    for (size_t j = 0; j < oplen; j++) {
        if (toupper((unsigned char)cp->input[cp->pos + j]) != op[j]) return 0;
    }
    return oplen;
}

/* Forward declarations for recursive descent */
static int cond_or(cond_parser_t *cp);
static int cond_and(cond_parser_t *cp);
static int cond_not(cond_parser_t *cp);
static int cond_compare(cond_parser_t *cp);

/*
 * Collect a comparison operand (up to a comparison operator, .AND., .OR.,
 * .NOT., or ')').  Performs symbol/lexical substitution.
 */
static void cp_collect_operand(cond_parser_t *cp, char *buf, size_t bufsz)
{
    cp_skip_ws(cp);
    size_t start = cp->pos;
    size_t end   = cp->pos;

    /* Track parenthesis depth so we don't stop inside nested parens */
    int depth = 0;

    while (cp->pos < cp->len) {
        char c = cp->input[cp->pos];

        if (c == '(') { depth++; cp->pos++; end = cp->pos; continue; }
        if (c == ')') {
            if (depth == 0) break;
            depth--;
            cp->pos++;
            end = cp->pos;
            continue;
        }

        /* Check for any dot operator at depth 0 */
        if (depth == 0 && c == '.') {
            static const char *dot_ops[] = {
                ".EQS.", ".NES.", ".LTS.", ".GTS.", ".LES.", ".GES.",
                ".EQ.",  ".NE.",  ".LT.",  ".GT.",  ".LE.",  ".GE.",
                ".AND.", ".OR.",  ".NOT.",
            };
            int found = 0;
            for (size_t i = 0; i < sizeof(dot_ops)/sizeof(dot_ops[0]); i++) {
                if (cp_match_op(cp, dot_ops[i])) { found = 1; break; }
            }
            if (found) break;
        }

        cp->pos++;
        end = cp->pos;
    }

    /* Copy and trim */
    size_t len = end - start;
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, cp->input + start, len);
    buf[len] = '\0';

    /* Trim trailing whitespace */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
}

/*
 * Evaluate a single operand string: substitute symbols, lexical fns,
 * unquote, return in buf.
 */
static void cp_eval_operand(cond_parser_t *cp, const char *raw,
                            char *buf, size_t bufsz)
{
    /* Symbol substitution */
    char subst[DCL_MAX_VALUE];
    dcl_sym_substitute(raw, subst, sizeof(subst));

    /* Trim */
    char *s = subst;
    while (*s == ' ' || *s == '\t') s++;
    size_t slen = strlen(s);
    while (slen > 0 && (s[slen-1] == ' ' || s[slen-1] == '\t'))
        s[--slen] = '\0';

    /* Unquote */
    int was_quoted = 0;
    if (slen >= 2 && s[0] == '"' && s[slen-1] == '"') {
        s[slen-1] = '\0';
        s++;
        slen -= 2;
        was_quoted = 1;
    }

    /* Lexical function */
    if (!was_quoted && strncasecmp(s, "F$", 2) == 0 && cp->cctx) {
        dcl_eval_lexical(cp->cctx, s, buf, bufsz);
        return;
    }

    /* Automatic symbol substitution (VMS DCL "IF"/"WHILE" expressions):
     * an UNQUOTED operand that is a valid symbol name is replaced by its
     * value -- e.g. `IF P2 .EQS. ""` tests the value of P2, not the literal
     * "P2".  A quoted operand is always a literal string.  If the token is
     * not a defined symbol it is left as-is (OVMX keeps the literal rather
     * than raising %DCL-W-UNDSYM, matching the lenient integer-operand path
     * and parse_primary()). */
    if (!was_quoted && slen > 0 &&
        (isalpha((unsigned char)s[0]) || s[0] == '_' || s[0] == '$')) {
        int is_symname = 1;
        for (size_t k = 0; k < slen; k++) {
            char c = s[k];
            if (!(isalnum((unsigned char)c) || c == '_' || c == '$')) {
                is_symname = 0;
                break;
            }
        }
        if (is_symname) {
            const char *val = dcl_sym_get(s);
            if (val) {
                strncpy(buf, val, bufsz - 1);
                buf[bufsz - 1] = '\0';
                return;
            }
        }
    }

    strncpy(buf, s, bufsz - 1);
    buf[bufsz - 1] = '\0';
}

/*
 * OR — lowest precedence
 */
static int cond_or(cond_parser_t *cp)
{
    int left = cond_and(cp);
    while (1) {
        cp_skip_ws(cp);
        size_t adv = cp_match_op(cp, ".OR.");
        if (!adv) break;
        cp->pos += adv;
        int right = cond_and(cp);
        left = left || right;
    }
    return left;
}

/*
 * AND
 */
static int cond_and(cond_parser_t *cp)
{
    int left = cond_not(cp);
    while (1) {
        cp_skip_ws(cp);
        size_t adv = cp_match_op(cp, ".AND.");
        if (!adv) break;
        cp->pos += adv;
        int right = cond_not(cp);
        left = left && right;
    }
    return left;
}

/*
 * NOT
 */
static int cond_not(cond_parser_t *cp)
{
    cp_skip_ws(cp);
    size_t adv = cp_match_op(cp, ".NOT.");
    if (adv) {
        cp->pos += adv;
        return !cond_not(cp);
    }
    return cond_compare(cp);
}

/*
 * Comparison: operand [.OP. operand]
 */
static int cond_compare(cond_parser_t *cp)
{
    cp_skip_ws(cp);

    /* Parenthesised sub-expression */
    if (cp->pos < cp->len && cp->input[cp->pos] == '(') {
        cp->pos++; /* ( */
        int v = cond_or(cp);
        cp_skip_ws(cp);
        if (cp->pos < cp->len && cp->input[cp->pos] == ')') cp->pos++;
        return v;
    }

    /* Collect left operand */
    char lraw[DCL_MAX_VALUE];
    cp_collect_operand(cp, lraw, sizeof(lraw));

    /* Check for comparison operator */
    cp_skip_ws(cp);

    static const struct {
        const char *op;
        int is_string;
        int type;  /* 0=eq 1=ne 2=lt 3=gt 4=le 5=ge */
    } cmp_ops[] = {
        { ".EQS.", 1, 0 }, { ".NES.", 1, 1 },
        { ".LTS.", 1, 2 }, { ".GTS.", 1, 3 },
        { ".LES.", 1, 4 }, { ".GES.", 1, 5 },
        { ".EQ.",  0, 0 }, { ".NE.",  0, 1 },
        { ".LT.",  0, 2 }, { ".GT.",  0, 3 },
        { ".LE.",  0, 4 }, { ".GE.",  0, 5 },
    };

    for (size_t i = 0; i < sizeof(cmp_ops)/sizeof(cmp_ops[0]); i++) {
        size_t adv = cp_match_op(cp, cmp_ops[i].op);
        if (!adv) continue;
        cp->pos += adv;

        char rraw[DCL_MAX_VALUE];
        cp_collect_operand(cp, rraw, sizeof(rraw));

        char lval[DCL_MAX_VALUE], rval[DCL_MAX_VALUE];
        cp_eval_operand(cp, lraw, lval, sizeof(lval));
        cp_eval_operand(cp, rraw, rval, sizeof(rval));

        if (cmp_ops[i].is_string) {
            int cmp = strcmp(lval, rval);
            switch (cmp_ops[i].type) {
                case 0: return (cmp == 0);
                case 1: return (cmp != 0);
                case 2: return (cmp < 0);
                case 3: return (cmp > 0);
                case 4: return (cmp <= 0);
                case 5: return (cmp >= 0);
            }
        } else {
            long lv, rv;
            /* Try to resolve as symbol if not purely numeric */
            char *ep;
            lv = strtol(lval, &ep, 0);
            if (*ep != '\0') {
                const char *sv = dcl_sym_get(lval);
                lv = sv ? strtol(sv, NULL, 0) : 0;
            }
            rv = strtol(rval, &ep, 0);
            if (*ep != '\0') {
                const char *sv = dcl_sym_get(rval);
                rv = sv ? strtol(sv, NULL, 0) : 0;
            }
            switch (cmp_ops[i].type) {
                case 0: return (lv == rv);
                case 1: return (lv != rv);
                case 2: return (lv < rv);
                case 3: return (lv > rv);
                case 4: return (lv <= rv);
                case 5: return (lv >= rv);
            }
        }
    }

    /* No comparison operator — treat as boolean value */
    char lval[DCL_MAX_VALUE];
    cp_eval_operand(cp, lraw, lval, sizeof(lval));

    if (strcasecmp(lval, "TRUE") == 0)  return 1;
    if (strcasecmp(lval, "FALSE") == 0) return 0;
    if (lval[0] == '\0') return 0;

    char *ep;
    long v = strtol(lval, &ep, 0);
    return (v != 0) ? 1 : 0;
}

/*
 * Evaluate a DCL IF condition string.
 * Returns 1 if true, 0 if false.
 *
 * Supports:
 *   .NOT. expr
 *   expr .AND. expr
 *   expr .OR. expr
 *   expr .EQ.|.NE.|.LT.|.GT.|.LE.|.GE. expr
 *   expr .EQS.|.NES.|.LTS.|.GTS.|.LES.|.GES. expr
 *   (expr)
 */
static int eval_condition(struct dcl_context *ctx, const char *expr)
{
    if (!expr) return 0;

    /* Symbol-substitute the entire condition first */
    char subst[DCL_MAX_LINE];
    dcl_sym_substitute(expr, subst, sizeof(subst));

    cond_parser_t cp;
    cp.input = subst;
    cp.pos   = 0;
    cp.len   = strlen(subst);
    cp.cctx  = ctx;

    return cond_or(&cp);
}

/*
 * Execute a symbol assignment.
 */
static int exec_assign(struct dcl_context *ctx, struct dcl_command *cmd)
{
    (void)ctx;

    int scope = DCL_SYM_LOCAL;
    const char *value = cmd->rest;

    /* Determine assignment type from label field */
    if (strcmp(cmd->label, "==") == 0 || strcmp(cmd->label, ":==") == 0) {
        scope = DCL_SYM_GLOBAL;
    }

    /* For := and :==, the value is a string (upcase and trim) */
    if (cmd->label[0] == ':') {
        char trimmed[DCL_MAX_VALUE];
        const char *v = value;
        while (*v == ' ' || *v == '\t') v++;
        strncpy(trimmed, v, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t len = strlen(trimmed);
        while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t'))
            trimmed[--len] = '\0';

        /* For := string assignment, upcase and compress spaces */
        char result[DCL_MAX_VALUE];
        size_t ri = 0;
        int in_space = 0;
        int in_quote = 0;
        for (size_t i = 0; trimmed[i] && ri < sizeof(result) - 1; i++) {
            if (trimmed[i] == '"') {
                in_quote = !in_quote;
                continue; /* Don't include quotes in result */
            }
            if (!in_quote && (trimmed[i] == ' ' || trimmed[i] == '\t')) {
                if (!in_space) {
                    result[ri++] = ' ';
                    in_space = 1;
                }
            } else {
                if (in_quote) {
                    result[ri++] = trimmed[i];
                } else {
                    result[ri++] = (char)toupper((unsigned char)trimmed[i]);
                }
                in_space = 0;
            }
        }
        result[ri] = '\0';

        dcl_sym_set(cmd->verb, result, scope);
    } else {
        /* Regular = or == assignment */
        char trimmed[DCL_MAX_VALUE];
        const char *v = value;
        while (*v == ' ' || *v == '\t') v++;
        strncpy(trimmed, v, sizeof(trimmed) - 1);
        trimmed[sizeof(trimmed) - 1] = '\0';
        size_t len = strlen(trimmed);
        while (len > 0 && (trimmed[len - 1] == ' ' || trimmed[len - 1] == '\t'))
            trimmed[--len] = '\0';

        /* Check if it's a lexical function with no operators */
        if (strncasecmp(trimmed, "F$", 2) == 0 && !has_arith_op(trimmed)) {
            char result[DCL_MAX_VALUE];
            dcl_eval_lexical(ctx, trimmed, result, sizeof(result));
            /* Try to store as integer */
            char *endp;
            long val = strtol(result, &endp, 0);
            if (*endp == '\0' && result[0] != '\0')
                dcl_sym_set_int(cmd->verb, (int32_t)val, scope);
            else
                dcl_sym_set(cmd->verb, result, scope);
        } else if (has_arith_op(trimmed)) {
            /* Expression evaluation (arithmetic or string ops) */
            expr_val_t result;
            eval_expr(ctx, trimmed, &result);
            if (result.is_string) {
                dcl_sym_set(cmd->verb, result.sval, scope);
            } else {
                dcl_sym_set_int(cmd->verb, (int32_t)result.ival, scope);
            }
        } else {
            /* Simple value: perform symbol substitution, then assign.
             * Note: eval_expr (above) also calls dcl_sym_substitute internally,
             * but that is a separate branch — no double substitution occurs here. */
            char subst[DCL_MAX_VALUE];
            dcl_sym_substitute(trimmed, subst, sizeof(subst));

            /* Remove surrounding quotes */
            size_t slen = strlen(subst);
            char *sv = subst;
            int was_quoted = 0;
            if (slen >= 2 && sv[0] == '"' && sv[slen - 1] == '"') {
                sv[slen - 1] = '\0';
                sv++;
                slen -= 2;
                was_quoted = 1;
            }

            /* Lexical function after substitution */
            if (!was_quoted && strncasecmp(sv, "F$", 2) == 0) {
                char result[DCL_MAX_VALUE];
                dcl_eval_lexical(ctx, sv, result, sizeof(result));
                char *endp;
                long val = strtol(result, &endp, 0);
                if (*endp == '\0' && result[0] != '\0')
                    dcl_sym_set_int(cmd->verb, (int32_t)val, scope);
                else
                    dcl_sym_set(cmd->verb, result, scope);
            } else {
                /* Try integer */
                char *endp;
                long val = strtol(sv, &endp, 0);
                if (*endp == '\0' && sv[0] != '\0') {
                    dcl_sym_set_int(cmd->verb, (int32_t)val, scope);
                } else {
                    /* Automatic symbol substitution on an `=` right-hand side:
                     * `A = B` assigns the VALUE of symbol B (VMS treats the RHS
                     * of `=`/`==` as an expression).  This is what makes the
                     * `SRC = P'N'` parameter-indexing idiom work: 'N' expands to
                     * "2" giving the token P2, which is then resolved to the
                     * value of parameter P2.  A quoted RHS stays literal; an
                     * undefined symbol is kept as-is (lenient, no %DCL-W-UNDSYM). */
                    const char *rv = NULL;
                    if (!was_quoted && sv[0] != '\0' &&
                        (isalpha((unsigned char)sv[0]) || sv[0] == '_' || sv[0] == '$')) {
                        int is_symname = 1;
                        for (char *q = sv; *q; q++) {
                            if (!(isalnum((unsigned char)*q) || *q == '_' || *q == '$')) {
                                is_symname = 0;
                                break;
                            }
                        }
                        if (is_symname)
                            rv = dcl_sym_get(sv);
                    }
                    if (rv) {
                        /* Preserve integer typing when the value is numeric. */
                        char *ep2;
                        long iv = strtol(rv, &ep2, 0);
                        if (*ep2 == '\0' && rv[0] != '\0')
                            dcl_sym_set_int(cmd->verb, (int32_t)iv, scope);
                        else
                            dcl_sym_set(cmd->verb, rv, scope);
                    } else {
                        dcl_sym_set(cmd->verb, sv, scope);
                    }
                }
            }
        }
    }

    return SS$_NORMAL;
}

/*
 * Execute a parsed DCL command line.
 * This is the main dispatch function.
 */
int dcl_execute_command(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (!cmd) return SS$_BADPARAM;

    /* Check if we're in a skipped IF block */
    if (ctx->if_depth > 0 && ctx->if_stack[ctx->if_depth - 1].skip) {
        /* Only process ELSE, ENDIF, and IF (for nesting) */
        if (strcasecmp(cmd->verb, "ELSE") != 0 &&
            strcasecmp(cmd->verb, "ENDIF") != 0 &&
            strcasecmp(cmd->verb, "IF") != 0) {
            return SS$_NORMAL;
        }
    }

    /* Handle different command types */
    switch (cmd->type) {
    case DCL_NODE_ASSIGN:
        return exec_assign(ctx, cmd);

    case DCL_NODE_COMMENT:
    case DCL_NODE_LABEL:
        return SS$_NORMAL;

    case DCL_NODE_COMMAND:
        break; /* Fall through to verb dispatch */

    default:
        break;
    }

    /* Handle special verbs that aren't in the command table */
    if (strcasecmp(cmd->verb, "IF") == 0) {
        /* Parse: IF condition THEN command
         * cmd->rest holds everything after "IF " on the line.
         * We split on the keyword THEN (case-insensitive, surrounded by spaces
         * or at start/end of tokens).  We do a case-insensitive scan for
         * " THEN " (or " THEN" at end) to find the split point.
         */
        char condition[DCL_MAX_LINE] = {0};
        char then_cmd[DCL_MAX_LINE] = {0};
        int found_then = 0;

        /* Search for THEN as a whole word in cmd->rest */
        const char *rest = cmd->rest;
        const char *p = rest;
        while (*p) {
            /* Look for THEN as a word boundary */
            if ((p == rest || p[-1] == ' ' || p[-1] == '\t') &&
                strncasecmp(p, "THEN", 4) == 0 &&
                (p[4] == '\0' || p[4] == ' ' || p[4] == '\t')) {
                /* Found THEN */
                size_t clen = (size_t)(p - rest);
                /* Trim trailing whitespace from condition */
                while (clen > 0 && (rest[clen-1] == ' ' || rest[clen-1] == '\t'))
                    clen--;
                if (clen >= sizeof(condition)) clen = sizeof(condition) - 1;
                memcpy(condition, rest, clen);
                condition[clen] = '\0';

                /* Skip "THEN" and leading whitespace */
                const char *after = p + 4;
                while (*after == ' ' || *after == '\t') after++;
                strncpy(then_cmd, after, sizeof(then_cmd) - 1);
                then_cmd[sizeof(then_cmd) - 1] = '\0';
                found_then = 1;
                break;
            }
            p++;
        }

        if (!found_then) {
            /* No THEN found — entire rest is the condition */
            strncpy(condition, rest, sizeof(condition) - 1);
            condition[sizeof(condition) - 1] = '\0';
        }

        int cond_result = eval_condition(ctx, condition);

        if (found_then && then_cmd[0]) {
            /* Single-line IF: IF cond THEN cmd */
            if (cond_result) {
                return dcl_execute_line(then_cmd);
            }
            return SS$_NORMAL;
        } else {
            /* Multi-line IF block */
            if (ctx->if_depth >= DCL_MAX_NEST) {
                dcl_error("DCL", 4, "NESTLEV",
                          "maximum IF nesting level exceeded");
                return SS$_BADPARAM;
            }
            ctx->if_stack[ctx->if_depth].in_if = 1;
            ctx->if_stack[ctx->if_depth].condition_true = cond_result;
            ctx->if_stack[ctx->if_depth].in_else = 0;
            ctx->if_stack[ctx->if_depth].skip = !cond_result;
            ctx->if_depth++;
            return SS$_NORMAL;
        }
    }

    if (strcasecmp(cmd->verb, "ELSE") == 0) {
        if (ctx->if_depth <= 0) {
            dcl_error("DCL", 2, "NOIFBLK", "ELSE without IF");
            return SS$_BADPARAM;
        }
        ctx->if_stack[ctx->if_depth - 1].in_else = 1;
        ctx->if_stack[ctx->if_depth - 1].skip =
            ctx->if_stack[ctx->if_depth - 1].condition_true;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "ENDIF") == 0) {
        if (ctx->if_depth <= 0) {
            dcl_error("DCL", 2, "NOIFBLK", "ENDIF without IF");
            return SS$_BADPARAM;
        }
        ctx->if_depth--;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "GOTO") == 0) {
        if (cmd->param_count < 1) {
            dcl_error("DCL", 2, "NOLAB", "no label specified in GOTO");
            return SS$_BADPARAM;
        }
        if (ctx->proc_depth < 0) {
            dcl_error("DCL", 2, "NOINTERACT",
                      "GOTO not allowed in interactive mode");
            return SS$_BADPARAM;
        }
        FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
        if (dcl_find_label(fp, cmd->params[0]) != 0) {
            dcl_error("DCL", 2, "USGOTO",
                      "target of GOTO not found - \\%s\\", cmd->params[0]);
            return SS$_BADPARAM;
        }
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "GOSUB") == 0) {
        if (cmd->param_count < 1) {
            dcl_error("DCL", 2, "NOLAB", "no label specified in GOSUB");
            return SS$_BADPARAM;
        }
        if (ctx->proc_depth < 0) {
            dcl_error("DCL", 2, "NOINTERACT",
                      "GOSUB not allowed in interactive mode");
            return SS$_BADPARAM;
        }
        if (ctx->gosub_depth >= DCL_MAX_NEST) {
            dcl_error("DCL", 4, "NESTLEV",
                      "maximum GOSUB nesting level exceeded");
            return SS$_BADPARAM;
        }
        /* Save return position */
        FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
        ctx->gosub_stack[ctx->gosub_depth].file_offset = ftell(fp);
        ctx->gosub_stack[ctx->gosub_depth].line_number =
            ctx->proc_stack[ctx->proc_depth].line_number;
        ctx->gosub_depth++;

        if (dcl_find_label(fp, cmd->params[0]) != 0) {
            ctx->gosub_depth--;
            dcl_error("DCL", 2, "USGOTO",
                      "target of GOSUB not found - \\%s\\", cmd->params[0]);
            return SS$_BADPARAM;
        }
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "RETURN") == 0) {
        if (ctx->gosub_depth <= 0) {
            dcl_error("DCL", 2, "NOGOSUB", "RETURN without GOSUB");
            return SS$_BADPARAM;
        }
        ctx->gosub_depth--;
        FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
        fseek(fp, ctx->gosub_stack[ctx->gosub_depth].file_offset, SEEK_SET);
        ctx->proc_stack[ctx->proc_depth].line_number =
            ctx->gosub_stack[ctx->gosub_depth].line_number;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "ON") == 0) {
        /* ON ERROR THEN GOTO label / ON ERROR THEN CONTINUE */
        /* ON SEVERE_ERROR THEN GOTO label / ON SEVERE_ERROR THEN CONTINUE */
        if (cmd->param_count < 3) {
            dcl_error("DCL", 2, "IVKEYW", "invalid ON command syntax");
            return SS$_BADPARAM;
        }

        int is_severe = (strcasecmp(cmd->params[0], "SEVERE_ERROR") == 0);
        /* params[0] = ERROR/SEVERE_ERROR, params[1] = THEN, params[2] = CONTINUE/GOTO */

        if (strcasecmp(cmd->params[2], "CONTINUE") == 0) {
            if (ctx->proc_depth >= 0) {
                if (is_severe)
                    ctx->proc_stack[ctx->proc_depth].on_severe = 1;
                else
                    ctx->proc_stack[ctx->proc_depth].on_error = 1;
            } else {
                ctx->on_error_continue = 1;
            }
        } else if (strcasecmp(cmd->params[2], "GOTO") == 0) {
            if (cmd->param_count >= 4) {
                if (ctx->proc_depth >= 0) {
                    if (is_severe) {
                        ctx->proc_stack[ctx->proc_depth].on_severe = 2;
                        strncpy(ctx->proc_stack[ctx->proc_depth].on_severe_label,
                                cmd->params[3],
                                sizeof(ctx->proc_stack[0].on_severe_label) - 1);
                    } else {
                        ctx->proc_stack[ctx->proc_depth].on_error = 2;
                        strncpy(ctx->proc_stack[ctx->proc_depth].on_error_label,
                                cmd->params[3],
                                sizeof(ctx->proc_stack[0].on_error_label) - 1);
                    }
                }
            }
        }
        return SS$_NORMAL;
    }

    /* @ procedure */
    if (strcmp(cmd->verb, "@") == 0) {
        if (cmd->param_count < 1) {
            dcl_error("DCL", 2, "IVVERB", "missing procedure name");
            return SS$_BADPARAM;
        }
        /* Pass P1-P8 */
        char *params[8] = {NULL};
        int pcount = cmd->param_count - 1;
        for (int i = 0; i < pcount && i < 8; i++) {
            params[i] = cmd->params[i + 1];
        }
        return dcl_execute_script(cmd->params[0], pcount, params);
    }

    /* Look up the verb in the command table */
    const struct dcl_verb *verb = dcl_find_verb(cmd->verb);
    if (verb) {
        /* Phase 1 keystone (docs/design-dcl-fidelity.md sec 4): validate the
         * parsed qualifiers against the verb's declared table BEFORE the
         * handler runs, so an unknown qualifier is rejected with the authentic
         * %DCL-W-IVQUAL (or %DCL-W-IVKEYW for a bad keyword) instead of being
         * silently accepted. For verbs not yet retrofit with a table
         * (verb->quals == NULL) this returns SS$_NORMAL and nothing changes. */
        int qstatus = dcl_validate_qualifiers(verb, cmd);
        if (qstatus != SS$_NORMAL) {
            ctx->last_status = (uint32_t)qstatus;
            ctx->last_severity = (uint32_t)(qstatus & 7);
            char sbuf[32];
            snprintf(sbuf, sizeof(sbuf), "%d", qstatus);
            dcl_sym_set("$STATUS", sbuf, DCL_SYM_GLOBAL);
            snprintf(sbuf, sizeof(sbuf), "%d", qstatus & 7);
            dcl_sym_set("$SEVERITY", sbuf, DCL_SYM_GLOBAL);
            return qstatus;
        }

        int status = verb->handler(cmd);
        ctx->last_status = (uint32_t)status;
        ctx->last_severity = (uint32_t)(status & 7);

        /* Update $STATUS and $SEVERITY symbols */
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", status);
        dcl_sym_set("$STATUS", buf, DCL_SYM_GLOBAL);
        snprintf(buf, sizeof(buf), "%d", status & 7);
        dcl_sym_set("$SEVERITY", buf, DCL_SYM_GLOBAL);

        return status;
    }

    /* Foreign command dispatch (vms-96e): not a builtin, but if the verb
     * is a DCL symbol whose value begins with '$', OpenVMS treats it as
     * a foreign command -- "SYM :== $image-spec" then bare "SYM args"
     * activates image-spec with args as P1-P8, the same as RUN would if
     * RUN forwarded parameters. See dcl_exec_foreign_command() in
     * dcl_cmd_process.c for the full semantics and documented OVMX
     * deviations. */
    const char *symval = dcl_sym_get(cmd->verb);
    if (symval && symval[0] == '$') {
        int status = dcl_exec_foreign_command(ctx, cmd, symval + 1);
        ctx->last_status = (uint32_t)status;
        ctx->last_severity = (uint32_t)(status & 7);

        char buf[32];
        snprintf(buf, sizeof(buf), "%d", status);
        dcl_sym_set("$STATUS", buf, DCL_SYM_GLOBAL);
        snprintf(buf, sizeof(buf), "%d", status & 7);
        dcl_sym_set("$SEVERITY", buf, DCL_SYM_GLOBAL);

        return status;
    }

    /* Command not found */
    dcl_error("DCL", 2, "IVVERB",
              "unrecognized command verb - check validity and spelling\n"
              " \\%s\\", cmd->verb);
    return SS$_IVVERB;
}

/*
 * Execute a raw DCL command line string.
 * Performs parsing, then dispatches.
 */
int dcl_execute_line(const char *line)
{
    if (!line) return SS$_BADPARAM;

    /* Skip empty/whitespace lines */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '!') return SS$_NORMAL;

    struct dcl_command cmd;
    if (dcl_parse_line(p, &cmd) != 0) {
        dcl_error("DCL", 2, "SYNTAX", "syntax error in command line");
        return SS$_BADPARAM;
    }

    if (cmd.type == DCL_NODE_COMMENT) return SS$_NORMAL;
    if (cmd.verb[0] == '\0' && cmd.label[0] != '\0') return SS$_NORMAL;
    if (cmd.verb[0] == '\0') return SS$_NORMAL;

    return dcl_execute_command(&cmd);
}
