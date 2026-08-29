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
extern int dcl_call_subroutine(const char *label, int argc, char **argv);
long dcl_parse_int(const char *s, int *ok);
void dcl_set_status(struct dcl_context *ctx, int status);

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
 * Parse a DCL integer literal, honouring the VMS radix prefixes and the
 * C-style 0x hex form. Recognised forms (after an optional leading sign):
 *   %X<hex>   %O<octal>   %D<decimal>   %B<binary>   (VMS radix operators)
 *   0x<hex>                                          (accepted for convenience)
 *   <decimal>
 * On return *ok is 1 iff the entire (trimmed) string was a valid integer in
 * the detected radix, else 0. This is what makes $STATUS work in expressions:
 * $STATUS is stored VMS-style as "%X00000001", so IF/arithmetic must read a
 * "%X" value as an integer. Reference: VSI OpenVMS User's Manual, "Radix
 * qualifiers (%B, %D, %O, %X)"; DCL Dictionary, "Expressions".
 */
long dcl_parse_int(const char *s, int *ok)
{
    if (ok) *ok = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    int base = 10;
    if (s[0] == '%' && s[1]) {
        char r = (char)toupper((unsigned char)s[1]);
        if (r == 'X') { base = 16; s += 2; }
        else if (r == 'O') { base = 8;  s += 2; }
        else if (r == 'D') { base = 10; s += 2; }
        else if (r == 'B') { base = 2;  s += 2; }
        else return 0;   /* %? — not a recognised radix */
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16; s += 2;
    }
    if (*s == '\0') return 0;
    char *endp;
    long v = strtol(s, &endp, base);
    /* Trailing whitespace is tolerated. */
    while (*endp == ' ' || *endp == '\t') endp++;
    if (*endp != '\0') return 0;
    if (ok) *ok = 1;
    return neg ? -v : v;
}

/*
 * Refresh $STATUS and $SEVERITY after a command completes. VMS stores $STATUS
 * as the 32-bit condition value rendered "%Xhhhhhhhh" (SHOW SYMBOL $STATUS
 * shows exactly that) and $SEVERITY as the low three bits in decimal. The two
 * are updated together, once, from the real completion status of every command
 * DCL runs — including a command it could not find. Centralised here so no
 * dispatch path can leave a stale success behind (the bug that made
 * IF .NOT. $STATUS silently miss a mistyped command). Reference: DCL
 * Dictionary, "$STATUS" and "$SEVERITY".
 */
void dcl_set_status(struct dcl_context *ctx, int status)
{
    if (ctx) {
        ctx->last_status = (uint32_t)status;
        ctx->last_severity = (uint32_t)(status & 7);
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%%X%08X", (uint32_t)status);
    dcl_sym_set("$STATUS", buf, DCL_SYM_GLOBAL);
    snprintf(buf, sizeof(buf), "%u", (uint32_t)status & 7);
    dcl_sym_set("$SEVERITY", buf, DCL_SYM_GLOBAL);
}

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
        /* Try integer (radix-aware: a "%X..." $STATUS-style value is numeric) */
        int iok;
        long iv = dcl_parse_int(val, &iok);
        if (iok) return make_int(iv);
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
        int iok;
        long iv = dcl_parse_int(val, &iok);
        if (iok) return make_int(iv);
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
        int iok;
        long iv = dcl_parse_int(result, &iok);
        /*
         * Coerce a lexical's result to an integer ONLY when the string is the
         * CANONICAL representation of that integer. A FORMATTED result -- e.g.
         * F$PID's zero-padded %08X pid "00000068", or its first pid "00000001" --
         * parses as an int (68, 1) but must be preserved as a STRING: collapsing
         * it drops the leading zeros so it no longer matches SHOW SYSTEM's hex pid
         * (and "00000001" renders as the bare "1"). Only "68"==itself round-trips;
         * "00000068"!="68" does not, so the pad is kept. rd vms-dee (NOT a VAX
         * arch-asymmetry -- this coercion is arch-neutral and stripped F$PID's
         * format on every arch). Canonical ints (F$LENGTH -> "5") still coerce.
         */
        if (iok) {
            char canon[32];
            snprintf(canon, sizeof(canon), "%ld", iv);
            if (strcmp(canon, result) == 0)
                return make_int(iv);
        }
        return make_str(result);
    }

    /* VMS radix literal: %X<hex> %O<octal> %D<dec> %B<binary> */
    if (c == '%' && ep->pos + 1 < ep->len) {
        char r = (char)toupper((unsigned char)ep->input[ep->pos + 1]);
        if (r == 'X' || r == 'O' || r == 'D' || r == 'B') {
            char buf[80];
            size_t bi = 0;
            buf[bi++] = ep_consume(ep);   /* % */
            buf[bi++] = ep_consume(ep);   /* radix letter */
            while (ep->pos < ep->len && bi < sizeof(buf) - 1) {
                char ch = ep->input[ep->pos];
                if (isalnum((unsigned char)ch)) { buf[bi++] = ch; ep->pos++; }
                else break;
            }
            buf[bi] = '\0';
            int iok;
            long iv = dcl_parse_int(buf, &iok);
            return make_int(iok ? iv : 0);
        }
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
            int iok;
            long iv = dcl_parse_int(val, &iok);
            if (iok) return make_int(iv);
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
 * Evaluate a DCL expression to its string value (public wrapper).
 *
 * This routes through the SAME evaluator used for the right-hand side of an
 * `=` assignment (parse_expr -> parse_primary), so callers such as WRITE and
 * any other DCL comma-expression-list get identical semantics:
 *   - "quoted"      -> literal string (quotes stripped),
 *   - F$xxx(...)    -> lexical function evaluated,
 *   - symbol / 'sym'-> resolved to the symbol value,
 *   - a + b, a - b  -> string concatenation / substring removal,
 *   - integer forms -> rendered as their decimal text.
 * Do NOT reimplement expression handling in callers — call this. (vms-65f)
 */
void dcl_eval_expr_string(struct dcl_context *ctx, const char *expr,
                          char *out, size_t outlen)
{
    if (!out || outlen == 0) return;
    expr_val_t v;
    eval_expr(ctx, expr, &v);
    char tmp[64];
    const char *s = val_to_str(&v, tmp, sizeof(tmp));
    strncpy(out, s, outlen - 1);
    out[outlen - 1] = '\0';
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
            int iok;
            /* Radix-aware, so a "%X..." $STATUS operand compares as a number.
             * If the operand is not itself an integer, resolve it as a symbol
             * and parse that value the same way. */
            lv = dcl_parse_int(lval, &iok);
            if (!iok) {
                const char *sv = dcl_sym_get(lval);
                lv = sv ? dcl_parse_int(sv, &iok) : 0;
            }
            rv = dcl_parse_int(rval, &iok);
            if (!iok) {
                const char *sv = dcl_sym_get(rval);
                rv = sv ? dcl_parse_int(sv, &iok) : 0;
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

    /* A bare numeric IF operand is TRUE iff its low bit is set (odd), FALSE if
     * even -- NOT "non-zero". This is the whole reason `IF $STATUS` detects
     * success and `IF .NOT. $STATUS` detects failure: VMS status codes are odd
     * on success, even on error. Reference: DCL Dictionary, "IF" (a numeric
     * expression is true when the low-order bit is 1). Radix-aware so the
     * "%X..." $STATUS form is read as an integer. */
    int iok;
    long v = dcl_parse_int(lval, &iok);
    if (!iok) {
        const char *sv = dcl_sym_get(lval);
        v = sv ? dcl_parse_int(sv, &iok) : 0;
    }
    return (v & 1) ? 1 : 0;
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

    /* Strip a trailing "!" comment from the assignment right-hand side: DCL
     * treats an unquoted "!" as a comment to end of line, on assignments as
     * well as commands (the command lexer already stops at "!", but the RHS is
     * collected raw as cmd->rest). Without this, MMK's setup assignments --
     * MMK___OPEN = "OPEN" !'F$VERIFY(0,0)' -- store the comment (and the apostrophe
     * text mangled by symbol substitution) as the symbol value. VSI OpenVMS
     * User's Manual, comments. A "!" inside a quoted string is literal. */
    char rhs[DCL_MAX_LINE];
    {
        int in_quote = 0;
        size_t ri = 0;
        for (const char *s = cmd->rest; *s && ri < sizeof(rhs) - 1; s++) {
            if (*s == '"') in_quote = !in_quote;
            else if (*s == '!' && !in_quote) break;
            rhs[ri++] = *s;
        }
        rhs[ri] = '\0';
    }
    const char *value = rhs;

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
        /* RETURN unwinds the innermost of: an open GOSUB at this level, or a
         * CALLed SUBROUTINE, or (at a procedure's top level) the procedure
         * itself -- where RETURN is equivalent to EXIT. gosub_base separates a
         * GOSUB opened *in this level* from a caller's still-open GOSUB.
         * Reference: DCL Dictionary, "RETURN". */
        int have_gosub = (ctx->proc_depth >= 0 &&
                          ctx->gosub_depth > ctx->proc_stack[ctx->proc_depth].gosub_base);
        if (have_gosub) {
            ctx->gosub_depth--;
            FILE *fp = ctx->proc_stack[ctx->proc_depth].fp;
            fseek(fp, ctx->gosub_stack[ctx->gosub_depth].file_offset, SEEK_SET);
            ctx->proc_stack[ctx->proc_depth].line_number =
                ctx->gosub_stack[ctx->gosub_depth].line_number;
            return SS$_NORMAL;
        }

        if (ctx->proc_depth < 0) {
            /* Interactive: RETURN has nothing to return from. */
            dcl_error("DCL", 2, "NOGOSUB", "RETURN without GOSUB");
            return SS$_BADPARAM;
        }

        /* Return from the current SUBROUTINE / procedure level with an optional
         * status. RETURN with no operand uses $STATUS. */
        int rstatus = (int)ctx->last_status;
        if (cmd->param_count >= 1 && cmd->params[0][0]) {
            int iok;
            long v = dcl_parse_int(cmd->params[0], &iok);
            if (!iok) {
                const char *sv = dcl_sym_get(cmd->params[0]);
                v = sv ? dcl_parse_int(sv, &iok) : 0;
            }
            if (iok) rstatus = (int)v;
        }
        ctx->return_status = rstatus;
        ctx->return_requested = 1;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "CALL") == 0) {
        if (cmd->param_count < 1) {
            dcl_error("DCL", 2, "NOLAB", "no subroutine label specified in CALL");
            return SS$_BADPARAM;
        }
        /* params[0] = label; params[1..] = P1..P8 passed to the subroutine. */
        char *params[8] = {NULL};
        int pcount = cmd->param_count - 1;
        for (int i = 0; i < pcount && i < 8; i++)
            params[i] = cmd->params[i + 1];
        return dcl_call_subroutine(cmd->params[0], pcount, params);
    }

    if (strcasecmp(cmd->verb, "SUBROUTINE") == 0) {
        /* The SUBROUTINE header of a CALLed block is a no-op marker. Reached in
         * NORMAL (fall-through) execution -- i.e. control ran into a subroutine
         * definition -- SUBROUTINE is equivalent to EXIT (DCL Dictionary). */
        if (ctx->proc_depth >= 0 && ctx->proc_stack[ctx->proc_depth].is_subroutine)
            return SS$_NORMAL;
        ctx->exit_requested = 1;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "ENDSUBROUTINE") == 0) {
        /* End of a CALLed subroutine: return to the caller with $STATUS.
         * Reached in normal flow (no active CALL), behave like EXIT. */
        if (ctx->proc_depth >= 0 && ctx->proc_stack[ctx->proc_depth].is_subroutine) {
            ctx->return_status = (int)ctx->last_status;
            ctx->return_requested = 1;
            return SS$_NORMAL;
        }
        ctx->exit_requested = 1;
        return SS$_NORMAL;
    }

    if (strcasecmp(cmd->verb, "ON") == 0) {
        /* ON WARNING|ERROR|SEVERE_ERROR THEN command
         *
         * Arms a ONE-SHOT error action at the current command level. When a
         * command then completes with $SEVERITY at or above the named threshold,
         * DCL runs the THEN command (commonly GOTO/EXIT/CONTINUE), and resets the
         * level to its default action (exit on error) -- a handler re-arms by
         * issuing another ON. The action is local to this command level.
         * Clean-room (Rule 8): VSI OpenVMS DCL Dictionary, "ON"; OpenVMS User's
         * Manual, "Controlling Error Conditions". Severity encoding (low 3 bits
         * of $STATUS): 0=WARNING, 1=SUCCESS, 2=ERROR, 3=INFO, 4=FATAL(SEVERE). */
        if (cmd->param_count < 3) {
            dcl_error("DCL", 2, "IVKEYW", "invalid ON command syntax");
            return SS$_BADPARAM;
        }

        int sev;
        if (strcasecmp(cmd->params[0], "WARNING") == 0)          sev = 0;
        else if (strcasecmp(cmd->params[0], "ERROR") == 0)       sev = 2;
        else if (strcasecmp(cmd->params[0], "SEVERE_ERROR") == 0) sev = 4;
        else {
            dcl_error("DCL", 2, "IVKEYW",
                      "invalid ON condition keyword - \\%s\\", cmd->params[0]);
            return SS$_BADPARAM;
        }

        /* Reconstruct the THEN command from params[2..] (params[1] == THEN). */
        char action[256];
        action[0] = '\0';
        for (int i = 2; i < cmd->param_count; i++) {
            if (action[0])
                strncat(action, " ", sizeof(action) - strlen(action) - 1);
            strncat(action, cmd->params[i], sizeof(action) - strlen(action) - 1);
        }

        if (ctx->proc_depth >= 0) {
            ctx->proc_stack[ctx->proc_depth].on_armed = 1;
            ctx->proc_stack[ctx->proc_depth].on_severity = sev;
            strncpy(ctx->proc_stack[ctx->proc_depth].on_action, action,
                    sizeof(ctx->proc_stack[0].on_action) - 1);
            ctx->proc_stack[ctx->proc_depth].on_action[
                sizeof(ctx->proc_stack[0].on_action) - 1] = '\0';
        } else {
            /* Interactive level has no procedure to abort; retained for parity. */
            ctx->on_error_continue = (strcasecmp(action, "CONTINUE") == 0);
            ctx->on_error_goto = 0;
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
            dcl_set_status(ctx, qstatus);
            return qstatus;
        }

        int status = verb->handler(cmd);
        dcl_set_status(ctx, status);
        return status;
    }

    /* Foreign command dispatch (vms-96e): not a builtin, but if the verb
     * is a DCL symbol whose value begins with '$', OpenVMS treats it as
     * a foreign command -- "SYM :== $image-spec" then bare "SYM args"
     * activates image-spec with args as P1-P8, the same as RUN would if
     * RUN forwarded parameters. See dcl_exec_foreign_command() in
     * dcl_cmd_process.c for the full semantics and documented OVMX
     * deviations.
     *
     * This is a FIRST-TOKEN (verb-position) translation, so it consults the
     * verb scope domain: SET SYMBOL/SCOPE=(...)/VERB (or the default /ALL)
     * governs whether outer-level locals / globals are reachable here, while
     * /GENERAL leaves this path untouched (OpenVMS DCL Dictionary, SET SYMBOL
     * /VERB /GENERAL /ALL). */
    const char *symval = dcl_sym_get_ex(cmd->verb, DCL_SYM_DOMAIN_VERB);
    if (symval && symval[0] == '$') {
        int status = dcl_exec_foreign_command(ctx, cmd, symval + 1);
        dcl_set_status(ctx, status);
        return status;
    }

    /* Verb-position symbol substitution (VMS DCL): when the first token is a
     * symbol whose value is a command string (not a '$'-foreign image, handled
     * above), DCL replaces the token with the symbol's value and re-scans the
     * line -- "SYM = ""WRITE""" then "SYM MMK___OUTPUT ..." runs
     * "WRITE MMK___OUTPUT ...". MMK's subprocess protocol relies on exactly this
     * (it defines MMK___OPEN/MMK___SET/MMK___WRITE as "OPEN"/"SET"/"WRITE" to run
     * its end-of-command-marker commands without F$VERIFY echo). The verb token
     * is replaced; the raw tail after it (including any /qualifier) is kept
     * verbatim. Reference: VSI OpenVMS User's Manual, "Symbol substitution in
     * command lines". Bounded to a small iteration depth like DCL's own. */
    if (symval && symval[0] != '\0') {
        static int subst_depth = 0;
        if (subst_depth >= 16) {
            subst_depth = 0;
            dcl_error("DCL", 2, "IVVERB",
                      "symbol substitution nested too deeply\n \\%s\\",
                      cmd->verb);
            return SS$_IVVERB;
        }
        char newline[DCL_MAX_LINE];
        const char *tail = cmd->raw_tail;
        if (tail[0] == '\0')
            snprintf(newline, sizeof(newline), "%s", symval);
        else if (tail[0] == '/')            /* qualifier abuts the verb: no space */
            snprintf(newline, sizeof(newline), "%s%s", symval, tail);
        else
            snprintf(newline, sizeof(newline), "%s %s", symval, tail);
        subst_depth++;
        int status = dcl_execute_line(newline);
        subst_depth--;
        return status;
    }

    /* Command not found. This path MUST refresh $STATUS/$SEVERITY too: a
     * mistyped command is the classic case where a script does
     * `IF .NOT. $STATUS THEN GOTO err`, and leaving a stale success here is
     * exactly the bug that made that check silently pass (parity-map 9.2). */
    dcl_error("DCL", 2, "IVVERB",
              "unrecognized command verb - check validity and spelling\n"
              " \\%s\\", cmd->verb);
    dcl_set_status(ctx, SS$_IVVERB);
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
