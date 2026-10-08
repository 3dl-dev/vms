/*
 * sys_fao.c - Formatted ASCII Output (FAO) System Services
 *
 * SYS$FAO and SYS$FAOL - VMS Formatted ASCII Output services.
 * Implements the VMS FAO directive language for formatted string output.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual, Chapter 26
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * These three read no state at all -- they transform the caller's control
 * string and arguments into the caller's output buffer. Whether OpenVMS
 * dispatches $FAO into the executive is NOT settled here; the register records
 * only where OVMX's answer comes from. Pin it to the oracle before quoting
 * these lines as a VMS match.
 *
 * THAT UNSETTLED QUESTION NOW HAS AN ITEM (vms-fab). These three cited vms-5b4,
 * the closed item that built the register; they cite vms-f90, whose outcome is
 * the pin itself, across the eight compute-only services -- these three plus
 * $NUMTIM/$ASCTIM/$BINTIM, $CHECK_FEN and $UNWIND. "Reads no system state" is a
 * reason to check, not a reason not to.
 *
 * OVMX-USERSPACE: sys$fao (vms-f90) -- formats into the caller's outbuf from
 *     the caller's varargs; reads no process, system or device state.
 * OVMX-USERSPACE: sys$faol (vms-f90) -- same, from a caller-supplied
 *     parameter list rather than varargs.
 * OVMX-USERSPACE: sys$fao_count_args (vms-f90) -- counts directives in the
 *     caller's control string. An OVMX-internal helper that took a sys$ name;
 *     the gate prints a "proto" column saying whether a header declares it.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "starlet.h"

/*
 * THE DIRECTIVE ENGINE (rd vms-546). Every directive the OpenVMS System
 * Services Reference documents for $FAO, with the field-width rules observed on
 * VAX V7.3 and Alpha V8.4 (docs/oracle/semantics/fao/):
 *
 *   !AS !AD !AC !AF   strings; a width left-justifies and blank-pads or
 *                     truncates; !AF shows a non-printable byte as '.'
 *   !UB/W/L !SB/W/L   decimal, right-justified in a width; too wide -> '*'s
 *   !ZB/W/L           decimal zero-filled to the width; minimal without one
 *   !XB/W/L !OB/W/L   hex / octal, zero-filled to the width (default 2/4/8,
 *                     3/6/11 digits); a value wider than the field keeps its
 *                     low-order digits
 *   !UQ !SQ !XQ !OQ !ZQ  the same for a quadword passed by reference
 *   !/ !_ !^ !!       CR LF, TAB, FF, '!'
 *   !n*c              c repeated n times (n from a parameter with no count)
 *   !n(DD)            the directive repeated n times
 *   !n<...!>          an output field of n characters for the text inside
 *   !- !+             reuse the previous parameter / skip the next one
 *   !%S               an 's' ('S' after an upper-case letter) unless the
 *                     last number converted was 1
 *   !%U !%I           a UIC [g,m] in octal / as its identifier name
 *   !%D !%T           date-time / time from a quadword ($ASCTIM)
 * An unknown directive, or a '!' that ends the string, stops the
 * conversion with SS$_BADPARAM; output that does not fit stops it with
 * SS$_BUFFEROVF. Either way the characters produced so far are kept and
 * counted in outlen.
 */

struct fao {
    char       *out;          /* the caller's buffer */
    size_t      cap, len;     /* its size and what has been written */
    const uint64_t *prm;      /* the parameter list (NULL: counting only) */
    int         idx, maxidx;  /* next parameter / one past the furthest used */
    uint64_t    lastnum;      /* the last number converted (!%S) */
    int         ovf;
};

static uint64_t fao_arg(struct fao *f)
{
    int i = f->idx++;
    if (f->idx > f->maxidx)
        f->maxidx = f->idx;
    return f->prm ? f->prm[i] : 0;
}

static void fao_putc(struct fao *f, char c)
{
    if (f->len >= f->cap) {
        f->ovf = 1;
        return;
    }
    if (f->out)
        f->out[f->len] = c;
    f->len++;
}

static void fao_putn(struct fao *f, const char *p, size_t n)
{
    for (size_t i = 0; i < n && !f->ovf; i++)
        fao_putc(f, p[i]);
}

/* Put `s` (n chars) into a field of `width` (-1 = no field): left-justified,
 * blank-padded, truncated to the field. */
static void fao_field_str(struct fao *f, const char *s, size_t n, int width)
{
    if (width >= 0 && n > (size_t)width)
        n = (size_t)width;
    fao_putn(f, s, n);
    for (int i = (int)n; width >= 0 && i < width && !f->ovf; i++)
        fao_putc(f, ' ');
}

static int fao_utoa(uint64_t v, int base, char *buf)
{
    char tmp[32];
    int n = 0;
    do {
        int d = (int)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return n;
}

/* A numeric conversion. kind: 'U','S','Z','X','O'; bytes: 1, 2, 4 or 8. */
static void fao_number(struct fao *f, char kind, int bytes, uint64_t raw, int width)
{
    char digits[40];
    int n, neg = 0;
    uint64_t v = bytes == 8 ? raw : raw & ((1ULL << (bytes * 8)) - 1);

    if (kind == 'S') {
        int64_t sv = bytes == 1 ? (int8_t)v : bytes == 2 ? (int16_t)v
                   : bytes == 4 ? (int32_t)v : (int64_t)v;
        neg = sv < 0;
        v = neg ? (uint64_t)0 - (uint64_t)sv : (uint64_t)sv;
        f->lastnum = (uint64_t)sv;
    } else {
        f->lastnum = v;
    }

    if (kind == 'X' || kind == 'O') {
        int base = kind == 'X' ? 16 : 8;
        int deflt = kind == 'X' ? bytes * 2
                  : bytes == 1 ? 3 : bytes == 2 ? 6 : bytes == 4 ? 11 : 22;
        int w = width >= 0 ? width : deflt;
        n = fao_utoa(v, base, digits);
        for (int i = n; i < w && !f->ovf; i++)
            fao_putc(f, '0');
        fao_putn(f, digits + (n > w ? n - w : 0), (size_t)(n > w ? w : n));
        return;
    }

    n = fao_utoa(v, 10, digits);
    if (width < 0) {
        if (neg)
            fao_putc(f, '-');
        fao_putn(f, digits, (size_t)n);
        return;
    }
    if (n + neg > width) {                         /* does not fit: asterisks */
        for (int i = 0; i < width && !f->ovf; i++)
            fao_putc(f, '*');
        return;
    }
    if (kind == 'Z') {
        if (neg)
            fao_putc(f, '-');
        for (int i = n + neg; i < width && !f->ovf; i++)
            fao_putc(f, '0');
    } else {
        for (int i = n + neg; i < width && !f->ovf; i++)
            fao_putc(f, ' ');
        if (neg)
            fao_putc(f, '-');
    }
    fao_putn(f, digits, (size_t)n);
}

static uint32_t fao_run(struct fao *f, const char *c, const char *end);

/* One directive at *pc (just past the '!'). */
static uint32_t fao_directive(struct fao *f, const char **pc, const char *end)
{
    const char *c = *pc;
    int count = -1;

    if (c >= end)
        return SS$_BADPARAM;                       /* a trailing '!' */
    if (*c == '#') {                               /* count from a parameter */
        count = (int)(uint32_t)fao_arg(f);
        c++;
    } else if (isdigit((unsigned char)*c)) {
        count = 0;
        while (c < end && isdigit((unsigned char)*c))
            count = count * 10 + (*c++ - '0');
    }
    if (c >= end)
        return SS$_BADPARAM;

    switch (*c) {
    case '/': *pc = c + 1; fao_putc(f, '\r'); fao_putc(f, '\n'); return SS$_NORMAL;
    case '_': *pc = c + 1; fao_putc(f, '\t'); return SS$_NORMAL;
    case '^': *pc = c + 1; fao_putc(f, '\f'); return SS$_NORMAL;
    case '!': *pc = c + 1; fao_putc(f, '!'); return SS$_NORMAL;
    case '-':
        *pc = c + 1;
        if (f->idx > 0)
            f->idx--;
        return SS$_NORMAL;
    case '+':
        *pc = c + 1;
        (void)fao_arg(f);
        return SS$_NORMAL;
    case '*': {
        if (c + 1 >= end)
            return SS$_BADPARAM;
        int n = count >= 0 ? count : (int)(uint32_t)fao_arg(f);
        for (int i = 0; i < n && !f->ovf; i++)
            fao_putc(f, c[1]);
        *pc = c + 2;
        return SS$_NORMAL;
    }
    case '(': {                                    /* !n(DD): repeat a directive */
        const char *close = memchr(c, ')', (size_t)(end - c));
        if (!close)
            return SS$_BADPARAM;
        int n = count >= 0 ? count : 1;
        for (int i = 0; i < n && !f->ovf; i++) {
            const char *inner = c + 1;
            uint32_t st = fao_directive(f, &inner, close);
            if (!(st & 1))
                return st;
            if (inner != close)
                return SS$_BADPARAM;
        }
        *pc = close + 1;
        return SS$_NORMAL;
    }
    case '<': {                                    /* !n<...!>: an output field */
        const char *p = c + 1, *close = NULL;
        int depth = 0;
        for (; p + 1 < end; p++) {
            if (p[0] == '!' && p[1] == '<') depth++;
            if (p[0] == '!' && p[1] == '>') {
                if (depth == 0) { close = p; break; }
                depth--;
            }
        }
        if (!close || count < 0)
            return SS$_BADPARAM;
        size_t start = f->len;
        uint32_t st = fao_run(f, c + 1, close);
        if (!(st & 1))
            return st;
        if (f->len > start + (size_t)count) {
            f->len = start + (size_t)count;        /* truncate to the field */
            f->ovf = 0;
        }
        while (f->len < start + (size_t)count && !f->ovf)
            fao_putc(f, ' ');
        *pc = close + 2;
        return SS$_NORMAL;
    }
    case '%': {
        if (c + 1 >= end)
            return SS$_BADPARAM;
        char k = (char)toupper((unsigned char)c[1]);
        *pc = c + 2;
        if (k == 'S') {
            if (f->lastnum != 1) {
                char prev = (f->out && f->len) ? f->out[f->len - 1] : 'a';
                fao_putc(f, isupper((unsigned char)prev) ? 'S' : 's');
            }
            return SS$_NORMAL;
        }
        if (k == 'U' || k == 'I') {
            uint32_t uic = (uint32_t)fao_arg(f);
            char buf[64];
            int n = -1;
            if (k == 'I' && f->prm) {
                char nm[32];
                uint16_t nl = 0;
                struct dsc$descriptor_s nd = { sizeof nm - 1, DSC$K_DTYPE_T,
                                               DSC$K_CLASS_S, nm };
                if ((sys$idtoasc(uic, &nl, &nd, NULL, NULL, NULL) & 1) && nl)
                    n = snprintf(buf, sizeof buf, "[%.*s]", (int)nl, nm);
            }
            if (n < 0)
                n = snprintf(buf, sizeof buf, "[%o,%o]", (unsigned)(uic >> 16),
                             (unsigned)(uic & 0xFFFF));
            fao_field_str(f, buf, (size_t)n, count);
            return SS$_NORMAL;
        }
        if (k == 'D' || k == 'T') {
            const uint64_t *t = (const uint64_t *)(uintptr_t)fao_arg(f);
            char buf[32];
            uint16_t tl = 0;
            struct dsc$descriptor_s td = { sizeof buf, DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S, buf };
            if (f->prm && (sys$asctim(&tl, &td, t, k == 'T') & 1))
                fao_field_str(f, buf, tl, count);
            return SS$_NORMAL;
        }
        return SS$_BADPARAM;
    }
    default:
        break;
    }

    /* Two-letter directives: a class letter and a size letter. */
    if (c + 1 >= end)
        return SS$_BADPARAM;
    char k = (char)toupper((unsigned char)c[0]);
    char z = (char)toupper((unsigned char)c[1]);
    *pc = c + 2;

    if (k == 'A') {
        const char *str = NULL;
        size_t n = 0;
        int af = 0;
        if (z == 'S') {
            const struct dsc$descriptor_s *d =
                (const struct dsc$descriptor_s *)(uintptr_t)fao_arg(f);
            if (d && f->prm) { str = d->dsc$a_pointer; n = d->dsc$w_length; }
        } else if (z == 'D' || z == 'F') {
            n = (size_t)(uint32_t)fao_arg(f);
            str = (const char *)(uintptr_t)fao_arg(f);
            af = z == 'F';
        } else if (z == 'C') {
            const unsigned char *cs = (const unsigned char *)(uintptr_t)fao_arg(f);
            if (cs && f->prm) { n = cs[0]; str = (const char *)cs + 1; }
        } else {
            return SS$_BADPARAM;
        }
        if (!f->prm || !str)
            n = 0;
        if (af) {
            char tmp[1024];
            size_t m = n < sizeof tmp ? n : sizeof tmp;
            for (size_t i = 0; i < m; i++) {
                unsigned char ch = (unsigned char)str[i];
                tmp[i] = (ch < 0x20 || ch >= 0x7F) ? '.' : (char)ch;
            }
            fao_field_str(f, tmp, m, count);
        } else {
            fao_field_str(f, str ? str : "", n, count);
        }
        return SS$_NORMAL;
    }

    if (k == 'U' || k == 'S' || k == 'Z' || k == 'X' || k == 'O') {
        int bytes = z == 'B' ? 1 : z == 'W' ? 2 : z == 'L' ? 4 : z == 'Q' ? 8 : 0;
        if (!bytes)
            return SS$_BADPARAM;
        uint64_t v = fao_arg(f);
        if (bytes == 8)                            /* a quadword by reference */
            v = (f->prm && v) ? *(const uint64_t *)(uintptr_t)v : 0;
        if (count == 0) {                          /* a zero-width field: nothing */
            f->lastnum = v;
            return SS$_NORMAL;
        }
        fao_number(f, k, bytes, v, count);
        return SS$_NORMAL;
    }
    return SS$_BADPARAM;
}

static uint32_t fao_run(struct fao *f, const char *c, const char *end)
{
    while (c < end && !f->ovf) {
        if (*c != '!') {
            fao_putc(f, *c++);
            continue;
        }
        c++;
        uint32_t st = fao_directive(f, &c, end);
        if (!(st & 1))
            return st;
    }
    return f->ovf ? SS$_BUFFEROVF : SS$_NORMAL;
}

/*
 * sys$faol - Formatted ASCII output with argument list
 */
uint32_t sys$faol(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    const uint64_t *prmlst)
{
    static const uint64_t noprm[1] = { 0 };

    if (!ctrstr || !outbuf) return SS$_BADPARAM;
    if (!ctrstr->dsc$a_pointer && ctrstr->dsc$w_length) return SS$_BADPARAM;
    if (!outbuf->dsc$a_pointer) return SS$_BADPARAM;

    struct fao f;
    memset(&f, 0, sizeof f);
    f.prm = prmlst ? prmlst : noprm;

    /* A dynamic output string (LIB$SYS_FAO's use) is sized to the result. */
    if (outbuf->dsc$b_class == DSC$K_CLASS_D) {
        char *tmp = malloc(65535);
        if (!tmp) return SS$_INSFMEM;
        f.out = tmp;
        f.cap = 65535;
        uint32_t st = fao_run(&f, ctrstr->dsc$a_pointer,
                              ctrstr->dsc$a_pointer + ctrstr->dsc$w_length);
        if (outbuf->dsc$w_length < f.len) {
            char *nb = realloc(outbuf->dsc$a_pointer, f.len);
            if (!nb) { free(tmp); return SS$_INSFMEM; }
            outbuf->dsc$a_pointer = nb;
        }
        memcpy(outbuf->dsc$a_pointer, tmp, f.len);
        outbuf->dsc$w_length = (uint16_t)f.len;
        free(tmp);
        if (outlen)
            *outlen = (uint16_t)f.len;
        return st;
    }

    f.out = outbuf->dsc$a_pointer;
    f.cap = outbuf->dsc$w_length;
    uint32_t st = fao_run(&f, ctrstr->dsc$a_pointer,
                          ctrstr->dsc$a_pointer + ctrstr->dsc$w_length);
    if (outlen)
        *outlen = (uint16_t)f.len;
    return st;
}

/* How many parameters a control string consumes (the furthest one a
 * directive, a repeat or !+ reaches). Also exported as sys$fao_count_args
 * for lib$sys_fao. */
int count_fao_args(const char *ctrl, uint16_t len) {
    struct fao f;
    memset(&f, 0, sizeof f);
    f.cap = (size_t)-1;                            /* never overflows */
    (void)fao_run(&f, ctrl, ctrl + len);
    return f.maxidx;
}

/*
 * sys$fao - Formatted ASCII output
 *
 * Implementation of the VMS SYS$FAO system service.
 * Wraps sys$faol by extracting arguments from the varargs list.
 */
uint32_t sys$fao(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    ...)
{
    if (!ctrstr || !ctrstr->dsc$a_pointer) return SS$_BADPARAM;

    /* Count actual directive arguments needed by the control string */
    int needed = count_fao_args(ctrstr->dsc$a_pointer, ctrstr->dsc$w_length);
    if (needed > 256) needed = 256;

    /* Build argument array from varargs — only read what's needed */
    uint64_t args[256];
    va_list ap;
    va_start(ap, outbuf);
    for (int i = 0; i < needed; i++) {
        args[i] = va_arg(ap, uint64_t);
    }
    va_end(ap);

    /* Call sys$faol with the argument array */
    return sys$faol(ctrstr, outlen, outbuf, args);
}

/* Exported wrapper so lib$sys_fao (lib_datetime.c) can count directives.
 * Uses a VMS-style name to match the naming convention. */
int sys$fao_count_args(const char *ctrl, uint16_t len) {
    return count_fao_args(ctrl, len);
}
