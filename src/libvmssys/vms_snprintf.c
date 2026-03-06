/*
 * vms_snprintf.c - Minimal snprintf implementation (no glibc)
 *
 * Supports:
 *   %d %i %u %x %X %o       - integer formats
 *   %ld %li %lu %lx %lX %lo - long variants
 *   %lld %llu %llx %llX     - long long variants
 *   %s %c %p %%             - string, char, pointer, literal %
 *   Width, precision, zero-pad, left-justify, space, plus flags
 *   %*d %.*s                 - width/precision from arguments
 */

#include "vms_snprintf.h"
#include "vms_string.h"

/* Output a single character to buffer if space remains.
 * Use pos + 1 < size instead of pos < size - 1 to avoid underflow when size==0. */
#define PUTC(ch) do {                    \
    if ((vms_size_t)pos + 1 < size)      \
        buf[pos] = (ch);                 \
    pos++;                               \
} while (0)

/* Output a string of given length */
static int put_str(char *buf, vms_size_t size, int pos, const char *s, int len)
{
    for (int i = 0; i < len; i++) {
        if ((vms_size_t)pos + 1 < size)
            buf[pos] = s[i];
        pos++;
    }
    return pos;
}

/* Output padding characters */
static int put_pad(char *buf, vms_size_t size, int pos, char c, int count)
{
    while (count-- > 0) {
        if ((vms_size_t)pos + 1 < size)
            buf[pos] = c;
        pos++;
    }
    return pos;
}

/* Convert unsigned integer to string, return length */
static int uint_to_str(char *out, unsigned long long val, int base, int upper)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;
    char tmp[24];
    int len = 0;

    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }

    while (val) {
        tmp[len++] = digits[val % (unsigned long long)base];
        val /= (unsigned long long)base;
    }

    for (int i = 0; i < len; i++)
        out[i] = tmp[len - 1 - i];
    out[len] = '\0';
    return len;
}

int vms_vsnprintf(char *buf, vms_size_t size, const char *fmt, va_list ap)
{
    int pos = 0;

    if (size == 0)
        return 0;

    while (*fmt) {
        if (*fmt != '%') {
            PUTC(*fmt);
            fmt++;
            continue;
        }

        fmt++; /* skip '%' */

        /* Parse flags */
        int flag_minus = 0, flag_plus = 0, flag_space = 0, flag_zero = 0, flag_hash = 0;
        for (;;) {
            if (*fmt == '-')      { flag_minus = 1; fmt++; }
            else if (*fmt == '+') { flag_plus = 1; fmt++; }
            else if (*fmt == ' ') { flag_space = 1; fmt++; }
            else if (*fmt == '0') { flag_zero = 1; fmt++; }
            else if (*fmt == '#') { flag_hash = 1; fmt++; }
            else break;
        }
        (void)flag_hash;

        /* Parse width */
        int width = 0;
        int has_width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flag_minus = 1;
                width = -width;
            }
            has_width = 1;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                has_width = 1;
                fmt++;
            }
        }
        (void)has_width;

        /* Parse precision */
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                if (precision < 0)
                    precision = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Parse length modifier */
        int length = 0;  /* 0=int, 1=long, 2=long long */
        if (*fmt == 'l') {
            length = 1;
            fmt++;
            if (*fmt == 'l') {
                length = 2;
                fmt++;
            }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h')
                fmt++;
        } else if (*fmt == 'z') {
            length = 1;  /* size_t = long on x86_64 */
            fmt++;
        }

        /* Format specifier */
        char numbuf[24];
        int numlen;
        char sign = 0;
        char padchar = (flag_zero && !flag_minus) ? '0' : ' ';

        switch (*fmt) {
        case 'd':
        case 'i': {
            long long val;
            if (length == 2)      val = va_arg(ap, long long);
            else if (length == 1) val = va_arg(ap, long);
            else                  val = va_arg(ap, int);

            if (val < 0) {
                sign = '-';
                val = -val;
            } else if (flag_plus) {
                sign = '+';
            } else if (flag_space) {
                sign = ' ';
            }

            numlen = uint_to_str(numbuf, (unsigned long long)val, 10, 0);

            /* Apply precision (minimum digits) */
            int num_zeros = 0;
            if (precision >= 0 && precision > numlen)
                num_zeros = precision - numlen;

            int total = numlen + num_zeros + (sign ? 1 : 0);
            int pad = (width > total) ? width - total : 0;

            /* If zero-padding with precision, use space pad */
            if (precision >= 0)
                padchar = ' ';

            if (!flag_minus) {
                if (padchar == '0' && sign) {
                    PUTC(sign);
                    sign = 0;
                }
                pos = put_pad(buf, size, pos, padchar, pad);
            }
            if (sign)
                PUTC(sign);
            pos = put_pad(buf, size, pos, '0', num_zeros);
            pos = put_str(buf, size, pos, numbuf, numlen);
            if (flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            break;
        }

        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long val;
            if (length == 2)      val = va_arg(ap, unsigned long long);
            else if (length == 1) val = (unsigned long)va_arg(ap, unsigned long);
            else                  val = (unsigned int)va_arg(ap, unsigned int);

            int base = (*fmt == 'x' || *fmt == 'X') ? 16 : (*fmt == 'o' ? 8 : 10);
            int upper = (*fmt == 'X');

            numlen = uint_to_str(numbuf, val, base, upper);

            int num_zeros = 0;
            if (precision >= 0 && precision > numlen)
                num_zeros = precision - numlen;

            int total = numlen + num_zeros;
            int pad = (width > total) ? width - total : 0;

            if (precision >= 0)
                padchar = ' ';

            if (!flag_minus)
                pos = put_pad(buf, size, pos, padchar, pad);
            pos = put_pad(buf, size, pos, '0', num_zeros);
            pos = put_str(buf, size, pos, numbuf, numlen);
            if (flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            break;
        }

        case 'p': {
            unsigned long long val = (unsigned long long)(unsigned long)va_arg(ap, void *);
            numbuf[0] = '0';
            numbuf[1] = 'x';
            numlen = uint_to_str(numbuf + 2, val, 16, 0) + 2;

            int pad = (width > numlen) ? width - numlen : 0;
            if (!flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            pos = put_str(buf, size, pos, numbuf, numlen);
            if (flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            break;
        }

        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";

            int slen = (int)vms_strlen(s);
            if (precision >= 0 && precision < slen)
                slen = precision;

            int pad = (width > slen) ? width - slen : 0;
            if (!flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            pos = put_str(buf, size, pos, s, slen);
            if (flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            break;
        }

        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            PUTC(c);
            if (flag_minus)
                pos = put_pad(buf, size, pos, ' ', pad);
            break;
        }

        case '%':
            PUTC('%');
            break;

        case '\0':
            goto done;

        default:
            PUTC('%');
            PUTC(*fmt);
            break;
        }
        fmt++;
    }

done:
    if (size > 0)
        buf[(vms_size_t)pos < size ? (vms_size_t)pos : size - 1] = '\0';
    return pos;
}

int vms_snprintf(char *buf, vms_size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vms_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int vms_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vms_vsnprintf(buf, (vms_size_t)-1, fmt, ap);
    va_end(ap);
    return ret;
}
