/*
 * lib_tparse.c - LIB$TABLE_PARSE / LIB$TPARSE table-driven finite-state parser
 *
 * A real TPARSE engine (bead vms-9f6): the description-file / object-list
 * parsers in MMK (bead vms-486) and the CLD/DCL toolchain drive their grammars
 * through this routine.  This replaces the former 14-line stub that returned
 * SS$_UNSUPPORTED and never defined lib$table_parse at all.
 *
 * CLEAN-ROOM (Rule 8): the SEMANTICS below are derived only from public VSI
 * OpenVMS documentation — the RTL Library Reference entry for LIB$TABLE_PARSE /
 * LIB$TPARSE, the TPA$ special token classes, and the action-routine argument
 * block (the "TPA block").  No VSI/HPE source or binary was consulted.  The
 * in-memory state/key table LAYOUT is not published by VSI, so OVMX defines its
 * own (TPA_GRAMMAR/TPA_STATE/TPA_TRAN in tpadef.h) and labels it an OVMX design
 * choice there.  What matches VMS is the observable behaviour, not the bytes.
 *
 * Engine summary (public LIB$TABLE_PARSE semantics):
 *   - The parser is a finite-state machine.  Each state holds an ordered list
 *     of transitions.  In a state, transitions are tried in order.
 *   - A transition matches the input against one of:
 *       * a literal character,
 *       * a keyword string (case-blind; optional abbreviation, TPA$M_ABBREV),
 *       * a special token class (TPA$_ALPHA/DIGIT/SYMBOL/STRING/ANY/BLANK/EOS/
 *         DECIMAL/OCTAL/HEX/UIC/LAMBDA),
 *       * a subexpression (recurse into another state; matches iff it reaches
 *         TPA$_EXIT).
 *   - On a match the engine records the token (tpa$l_tokenptr/tpa$l_tokencnt,
 *     and tpa$l_number for numeric classes), advances the input, stores the
 *     transition parameter in tpa$l_param, and — if present — calls the action
 *     routine with the address of the TPA block.  An action returning an EVEN
 *     (failure) status REJECTS the transition: the input is backtracked and the
 *     next transition is tried.  With TPA$M_BLANKS set, blanks between tokens
 *     are skipped automatically.
 *   - The next state is a state index, or TPA$_EXIT (parse succeeds,
 *     SS$_NORMAL) or TPA$_FAIL (parse fails, LIB$_SYNTAXERR).  A state with no
 *     matching transition also yields LIB$_SYNTAXERR.
 */

#include <stdint.h>
#include <stddef.h>
#include "ssdef.h"
#include "tpadef.h"
#include "lib$routines.h"   /* LIB$_SYNTAXERR, LIB$_INVARG (via libdef.h), lib$ decls */

#ifndef $VMS_STATUS_SUCCESS
#define $VMS_STATUS_SUCCESS(code)   ((code) & 1)
#endif

/* Guard against runaway grammars (a mis-authored table with a transition cycle
 * that consumes no input).  The bound scales with the input so legitimately
 * long inputs still parse; a true no-progress loop trips it. */
#define TPA_MAX_STEPS_BASE  4096u

/* Input cursor + engine context, threaded through the recursion. */
typedef struct {
    TPADEF            *tpa;        /* standard TPARSE block (first 8 longwords) */
    void              *block;      /* ORIGINAL block ptr handed to action routines */
    const TPA_GRAMMAR *g;
    uint32_t           blanks;     /* TPA$M_BLANKS active */
    uint32_t           abbrev;     /* TPA$M_ABBREV active */
    uint32_t           steps;      /* progress budget (see TPA_MAX_STEPS_BASE) */
    uint32_t           step_limit;
} tpa_ctx;

/* ---- character classifiers (locale-independent; -ffreestanding safe) ---- */
static int tpa_is_blank(char c) { return c == ' ' || c == '\t'; }
static int tpa_is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static int tpa_is_lower(char c) { return c >= 'a' && c <= 'z'; }
static int tpa_is_alpha(char c) { return tpa_is_upper(c) || tpa_is_lower(c); }
static int tpa_is_digit(char c) { return c >= '0' && c <= '9'; }
static int tpa_is_odigit(char c) { return c >= '0' && c <= '7'; }
static int tpa_is_alnum(char c) { return tpa_is_alpha(c) || tpa_is_digit(c); }
static int tpa_is_symchar(char c) { return tpa_is_alnum(c) || c == '$' || c == '_'; }
static char tpa_upcase(char c) { return tpa_is_lower(c) ? (char)(c - 'a' + 'A') : c; }

static int tpa_hexval(char c) {
    if (tpa_is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Skip leading blanks when TPA$M_BLANKS is active. */
static void tpa_skip_blanks(tpa_ctx *ctx) {
    if (!ctx->blanks) return;
    TPADEF *t = ctx->tpa;
    while (t->tpa$l_stringcnt > 0 && tpa_is_blank(t->tpa$l_stringptr[0])) {
        t->tpa$l_stringptr++;
        t->tpa$l_stringcnt--;
    }
}

/* Consume n chars, recording the token span at the CURRENT position. */
static void tpa_take(tpa_ctx *ctx, char *start, uint32_t n) {
    TPADEF *t = ctx->tpa;
    t->tpa$l_tokenptr = start;
    t->tpa$l_tokencnt = n;
    t->tpa$l_stringptr = start + n;
    /* stringcnt was measured from `start`; subtract what we consumed. */
    t->tpa$l_stringcnt = t->tpa$l_stringcnt - n;
}

static uint32_t tpa_run(tpa_ctx *ctx, uint32_t state_index);

/*
 * Attempt to match a single transition's TYPE against the input at the current
 * position.  On success the input is advanced, the token fields are set, and 1
 * is returned; on failure the position is left UNCHANGED and 0 is returned.
 * (The caller still saves/restores across the action-routine callback.)
 */
static int tpa_match(tpa_ctx *ctx, const TPA_TRAN *tr) {
    TPADEF  *t = ctx->tpa;
    char    *base;
    uint32_t avail;

    /* Two transition types must NOT pre-skip blanks:
     *   - TPA$_BLANK matches blanks explicitly; and
     *   - TPA$_LAMBDA is a null (zero-width) transition — pre-skipping would
     *     let a terminal lambda silently CONSUME trailing blanks that a later
     *     explicit TPA$_BLANK still needs.  In MMK, a line's leading blank
     *     (CONTINUE routes "  cmd" to COMMAND) and a symbol's trailing blank
     *     (SYMBOL1 -> lambda -> EXIT) both depend on lambda not eating blanks.
     *     A null transition consumes no input, blanks included.  (bead vms-486) */
    if (tr->type != TPA$_BLANK && tr->type != TPA$_LAMBDA)
        tpa_skip_blanks(ctx);

    base  = t->tpa$l_stringptr;
    avail = t->tpa$l_stringcnt;

    /* Literal character (type is a byte value 0..255). */
    if (tr->type <= 0xFFu) {
        if (avail > 0 && (unsigned char)base[0] == (unsigned char)tr->type) {
            tpa_take(ctx, base, 1);
            return 1;
        }
        return 0;
    }

    switch (tr->type) {

    case TPA$_LAMBDA:                 /* always matches, zero length */
        tpa_take(ctx, base, 0);
        return 1;

    case TPA$_EOS:                    /* end of string */
        if (avail == 0) { tpa_take(ctx, base, 0); return 1; }
        return 0;

    case TPA$_ANY:                    /* any single character */
        if (avail > 0) { tpa_take(ctx, base, 1); return 1; }
        return 0;

    case TPA$_BLANK: {                /* one or more blanks */
        uint32_t n = 0;
        while (n < avail && tpa_is_blank(base[n])) n++;
        if (n == 0) return 0;
        tpa_take(ctx, base, n);
        return 1;
    }

    case TPA$_ALPHA: {               /* one or more alphabetics */
        uint32_t n = 0;
        while (n < avail && tpa_is_alpha(base[n])) n++;
        if (n == 0) return 0;
        tpa_take(ctx, base, n);
        return 1;
    }

    case TPA$_DIGIT: {               /* one or more digits (no numeric value) */
        uint32_t n = 0;
        while (n < avail && tpa_is_digit(base[n])) n++;
        if (n == 0) return 0;
        tpa_take(ctx, base, n);
        return 1;
    }

    case TPA$_SYMBOL: {              /* alnum/$/_ identifier, must start non-digit */
        uint32_t n = 0;
        if (avail == 0 || tpa_is_digit(base[0]) || !tpa_is_symchar(base[0]))
            return 0;
        while (n < avail && tpa_is_symchar(base[n])) n++;
        tpa_take(ctx, base, n);
        return 1;
    }

    case TPA$_STRING:               /* the entire remainder of the input */
        if (avail == 0) return 0;
        tpa_take(ctx, base, avail);
        return 1;

    case TPA$_DECIMAL:
    case TPA$_OCTAL:
    case TPA$_HEX: {                /* unsigned integer; sets tpa$l_number */
        uint32_t n = 0, val = 0;
        int      radix = (tr->type == TPA$_DECIMAL) ? 10
                       : (tr->type == TPA$_OCTAL)   ? 8 : 16;
        while (n < avail) {
            int d;
            if (radix == 16) { d = tpa_hexval(base[n]); if (d < 0) break; }
            else if (radix == 8)  { if (!tpa_is_odigit(base[n])) break; d = base[n]-'0'; }
            else                  { if (!tpa_is_digit(base[n]))  break; d = base[n]-'0'; }
            val = val * (uint32_t)radix + (uint32_t)d;
            n++;
        }
        if (n == 0) return 0;
        t->tpa$l_number = val;
        tpa_take(ctx, base, n);
        return 1;
    }

    case TPA$_UIC: {               /* [group,member] in octal -> (g<<16)|m */
        uint32_t n = 0, grp = 0, mbr = 0, gd = 0, md = 0;
        if (avail < 5 || base[0] != '[') return 0;   /* [g,m] is >= 5 chars */
        n = 1;
        while (n < avail && tpa_is_odigit(base[n])) { grp = grp*8 + (uint32_t)(base[n]-'0'); gd++; n++; }
        if (gd == 0 || n >= avail || base[n] != ',') return 0;
        n++;
        while (n < avail && tpa_is_odigit(base[n])) { mbr = mbr*8 + (uint32_t)(base[n]-'0'); md++; n++; }
        if (md == 0 || n >= avail || base[n] != ']') return 0;
        n++;                                         /* consume ']' */
        t->tpa$l_number = (grp << 16) | (mbr & 0xFFFFu);
        tpa_take(ctx, base, n);
        return 1;
    }

    case TPA$K_KEYWORD: {          /* case-blind keyword; optional abbreviation */
        const char *kw = tr->keyword;
        uint32_t    klen = 0, n = 0;
        if (kw == NULL) return 0;
        while (kw[klen] != '\0') klen++;
        while (n < avail && n < klen && tpa_upcase(base[n]) == tpa_upcase(kw[n]))
            n++;
        if (n == klen &&
            (klen == avail || !tpa_is_symchar(base[klen]))) {
            /* Full keyword present AND terminated at a word boundary.  A
             * following symbol-constituent character means the input is a
             * LONGER identifier, so the shorter keyword must NOT match here.
             * This is the observable VMS LIB$TABLE_PARSE behaviour that lets a
             * table list 'SUFFIXES' ahead of 'SUFFIXES_AFTER' / 'SUFFIXES_*'
             * (MMK V2.8, bead vms-486) without the longer keywords becoming
             * unreachable dead transitions.  (Clean-room: derived from the
             * documented keyword-match semantics and the MMK table's ordering,
             * not from any VSI source.) */
            tpa_take(ctx, base, klen);
            return 1;
        }
        /* Abbreviation: input exhausted (or a delimiter) after matching a
         * non-empty leading substring of the keyword, and TPA$M_ABBREV set. */
        if (ctx->abbrev && n > 0 &&
            (n == avail || !tpa_is_symchar(base[n]))) {
            tpa_take(ctx, base, n);
            return 1;
        }
        return 0;
    }

    case TPA$K_SUBEXPR: {          /* recurse: match iff subexpression EXITs */
        char    *sp_save  = t->tpa$l_stringptr;
        uint32_t cnt_save = t->tpa$l_stringcnt;
        uint32_t st       = tpa_run(ctx, tr->target);
        if ($VMS_STATUS_SUCCESS(st)) {
            /* Token = the whole span the subexpression consumed. */
            t->tpa$l_tokenptr = sp_save;
            t->tpa$l_tokencnt = cnt_save - t->tpa$l_stringcnt;
            return 1;
        }
        /* Subexpression failed: restore the cursor for the next transition. */
        t->tpa$l_stringptr = sp_save;
        t->tpa$l_stringcnt = cnt_save;
        return 0;
    }

    default:
        return 0;              /* unknown type never matches */
    }
}

/*
 * Run the FSM starting at state_index.  Returns SS$_NORMAL on TPA$_EXIT,
 * LIB$_SYNTAXERR on TPA$_FAIL / no-match / runaway.
 */
static uint32_t tpa_run(tpa_ctx *ctx, uint32_t state_index) {
    TPADEF *t = ctx->tpa;

    for (;;) {
        if (state_index >= ctx->g->nstates)
            return LIB$_SYNTAXERR;
        if (ctx->steps++ >= ctx->step_limit)
            return LIB$_SYNTAXERR;             /* no-progress / cyclic table */

        const TPA_STATE *state = &ctx->g->states[state_index];
        uint32_t         i;
        int              took = 0;

        for (i = 0; i < state->ntrans; i++) {
            const TPA_TRAN *tr = &state->trans[i];
            char    *sp_save  = t->tpa$l_stringptr;
            uint32_t cnt_save = t->tpa$l_stringcnt;

            if (!tpa_match(ctx, tr)) {
                /* A failed match must consume no input.  tpa_match may have
                 * pre-skipped blanks (TPA$M_BLANKS) before discovering the
                 * mismatch; undo that so a LATER transition in this state — an
                 * explicit TPA$_BLANK, in particular — still sees those blanks.
                 * Without this, a state like MMK's CMD_PREFIXED (its '@'/'-'
                 * transitions pre-skip the blank that its BLANK->EXIT
                 * terminator needs) can never terminate.  (bead vms-486) */
                t->tpa$l_stringptr = sp_save;
                t->tpa$l_stringcnt = cnt_save;
                continue;
            }

            /* Matched: expose the parameter, then run the action (if any).
             * An even status rejects the transition and backtracks. */
            t->tpa$l_param = tr->param;
            if (tr->action != NULL) {
                uint32_t ast = tr->action(ctx->block);
                if (!$VMS_STATUS_SUCCESS(ast)) {
                    t->tpa$l_stringptr = sp_save;
                    t->tpa$l_stringcnt = cnt_save;
                    continue;
                }
            }

            /* Transition accepted — dispatch on the next-state field. */
            if (tr->next == TPA$_EXIT)
                return SS$_NORMAL;
            if (tr->next == TPA$_FAIL)
                return LIB$_SYNTAXERR;
            if (tr->next == TPA$K_NEXT_SEQ)
                state_index = state_index + 1;
            else
                state_index = tr->next;
            took = 1;
            break;
        }

        if (!took)
            return LIB$_SYNTAXERR;              /* no transition matched */
    }
}

/*
 * lib$table_parse - the TPARSE engine.
 *
 * tparse_block : address of a TPADEF (possibly extended by the caller).  The
 *                caller sets tpa$l_count (>= TPA$K_COUNT0), tpa$l_options,
 *                tpa$l_stringcnt, tpa$l_stringptr before the call.
 * state_table  : address of a TPA_GRAMMAR (OVMX format, see tpadef.h).
 * key_table    : RESERVED in the OVMX table format; may be NULL.
 *
 * Returns SS$_NORMAL on a complete parse, LIB$_SYNTAXERR on a parse failure,
 * LIB$_INVARG on a malformed argument block or grammar.
 */
uint32_t lib$table_parse(void *tparse_block,
                         const void *state_table,
                         const void *key_table) {
    (void)key_table;   /* reserved in the OVMX table format */

    TPADEF            *tpa = (TPADEF *)tparse_block;
    const TPA_GRAMMAR *g   = (const TPA_GRAMMAR *)state_table;

    if (tpa == NULL || g == NULL)
        return LIB$_INVARG;
    if (tpa->tpa$l_count < TPA$K_COUNT0)
        return LIB$_INVARG;
    if (g->magic != TPA$K_GRAMMAR_MAGIC || g->nstates == 0 || g->states == NULL)
        return LIB$_INVARG;

    tpa_ctx ctx;
    ctx.tpa        = tpa;
    ctx.block      = tparse_block;
    ctx.g          = g;
    ctx.blanks     = (tpa->tpa$l_options & TPA$M_BLANKS) ? 1u : 0u;
    ctx.abbrev     = (tpa->tpa$l_options & TPA$M_ABBREV) ? 1u : 0u;
    ctx.steps      = 0;
    ctx.step_limit = TPA_MAX_STEPS_BASE + tpa->tpa$l_stringcnt * 4u;

    /* Token fields start empty (a grammar that EXITs immediately reports none). */
    tpa->tpa$l_tokenptr = tpa->tpa$l_stringptr;
    tpa->tpa$l_tokencnt = 0;
    tpa->tpa$l_number   = 0;

    return tpa_run(&ctx, 0);   /* states[0] is the start state */
}

/*
 * lib$tparse - the MACRO-32-era name for the same engine.  MMK aliases
 * lib$tparse -> lib$table_parse; we export BOTH names so either spelling
 * links.  (VMS distinguishes the two by how action-routine arguments are
 * passed; OVMX action routines always take the block address, matching the
 * LIB$TABLE_PARSE convention that MMK relies on.)
 */
uint32_t lib$tparse(void *tparse_block,
                    const void *state_table,
                    const void *key_table) {
    return lib$table_parse(tparse_block, state_table, key_table);
}
