/*
 * sys_fao.c - Formatted ASCII Output (FAO) System Services
 *
 * SYS$FAO and SYS$FAOL - VMS Formatted ASCII Output services.
 * Implements the VMS FAO directive language for formatted string output.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual, Chapter 26
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "starlet.h"

/* Maximum FAO output buffer size for internal operations */
#define FAO_MAX_INTERNAL_BUF 65536

/* Helper: append character to output buffer */
static inline int append_char(char **out, char **out_end, char c) {
    if (*out >= *out_end) return -1;
    *(*out)++ = c;
    return 0;
}

/* Helper: append string to output buffer */
static inline int append_string(char **out, char **out_end, const char *str, size_t len) {
    size_t available = *out_end - *out;
    if (available < len) return -1;
    memcpy(*out, str, len);
    *out += len;
    return 0;
}

/* Helper: parse numeric prefix from control string */
static int parse_number(const char **ctrl, const char *ctrl_end, int *value) {
    *value = 0;
    if (*ctrl >= ctrl_end || !isdigit((unsigned char)**ctrl))
        return 0;

    while (*ctrl < ctrl_end && isdigit((unsigned char)**ctrl)) {
        *value = (*value * 10) + ((**ctrl) - '0');
        (*ctrl)++;
    }
    return 1;
}

/* Helper: format integer to string */
static int format_integer(char *buf, size_t bufsz, int64_t value, int base, int width, char pad) {
    char tmp[128];
    int len = 0;
    int neg = 0;
    uint64_t uval;

    if (base == 10 && value < 0) {
        neg = 1;
        uval = (uint64_t)-(value + 1) + 1;  /* safe negation for INT64_MIN */
    } else {
        uval = (uint64_t)value;
    }

    /* Convert to string (reversed) */
    if (uval == 0) {
        tmp[len++] = '0';
    } else {
        while (uval > 0 && len < (int)sizeof(tmp) - 1) {
            int digit = uval % base;
            tmp[len++] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
            uval /= base;
        }
    }

    /* Add sign if needed */
    if (neg && pad != '0') {
        tmp[len++] = '-';
    }

    /* Pad if needed */
    while (len < width && len < (int)sizeof(tmp) - 1) {
        tmp[len++] = pad;
    }

    /* Add sign after padding for zero-padded negative numbers */
    if (neg && pad == '0' && len < (int)sizeof(tmp) - 1) {
        tmp[len++] = '-';
    }

    /* Reverse into output buffer */
    if ((size_t)len >= bufsz) return -1;
    for (int i = 0; i < len; i++) {
        buf[i] = tmp[len - 1 - i];
    }
    buf[len] = '\0';

    return len;
}

/* Helper: process a single FAO directive */
static uint32_t process_directive(
    const char **ctrl,
    const char *ctrl_end,
    char **out,
    char **out_end,
    const uint64_t **args,
    int64_t *last_numeric_arg)
{
    char directive[4] = {0};
    int dir_len = 0;
    int repeat_count = 0;
    int has_repeat = 0;

    /* Skip the '!' */
    (*ctrl)++;
    if (*ctrl >= ctrl_end) return SS$_BADPARAM;

    /* Check for numeric prefix */
    has_repeat = parse_number(ctrl, ctrl_end, &repeat_count);
    if (*ctrl >= ctrl_end) return SS$_BADPARAM;

    /* Handle special single-character directives first */
    char first = **ctrl;
    if (first == '/' || first == '_' || first == '!' ||
        first == '*' || first == '%') {
        directive[0] = first;
        dir_len = 1;
        (*ctrl)++;
        /* %S is a two-character directive starting with % */
        if (first == '%' && *ctrl < ctrl_end && toupper((unsigned char)**ctrl) == 'S') {
            directive[1] = 'S';
            dir_len = 2;
            (*ctrl)++;
        }
    } else {
        /* Parse alphabetic directive (AS, SL, UL, etc.) */
        while (*ctrl < ctrl_end && dir_len < 3 && isalpha((unsigned char)**ctrl)) {
            directive[dir_len++] = toupper((unsigned char)**ctrl);
            (*ctrl)++;
        }
    }

    if (dir_len == 0) {
        return SS$_BADPARAM;
    }

    /* Process directives */

    /* Single character directives with no args */
    if (strcmp(directive, "/") == 0) {
        return append_char(out, out_end, '\n') ? SS$_BUFFEROVF : SS$_NORMAL;
    }
    if (strcmp(directive, "_") == 0) {
        return append_char(out, out_end, '\t') ? SS$_BUFFEROVF : SS$_NORMAL;
    }
    if (strcmp(directive, "!") == 0) {
        return append_char(out, out_end, '!') ? SS$_BUFFEROVF : SS$_NORMAL;
    }

    /* Character repeat: !n*c */
    if (strcmp(directive, "*") == 0) {
        if (!has_repeat) {
            repeat_count = (int)(**args);
            (*args)++;
        }
        if (*ctrl >= ctrl_end) return SS$_BADPARAM;
        char ch = **ctrl;
        (*ctrl)++;
        for (int i = 0; i < repeat_count; i++) {
            if (append_char(out, out_end, ch)) return SS$_BUFFEROVF;
        }
        return SS$_NORMAL;
    }

    /* Plural 's' - !%S */
    if (strcmp(directive, "%S") == 0) {
        if (*last_numeric_arg != 1) {
            if (append_char(out, out_end, 's')) return SS$_BUFFEROVF;
        }
        return SS$_NORMAL;
    }

    /* ASCII string from descriptor - !AS */
    if (strcmp(directive, "AS") == 0) {
        struct dsc$descriptor_s *desc = (struct dsc$descriptor_s *)(uintptr_t)(**args);
        (*args)++;
        if (desc && desc->dsc$a_pointer && desc->dsc$w_length > 0) {
            if (append_string(out, out_end, desc->dsc$a_pointer, desc->dsc$w_length))
                return SS$_BUFFEROVF;
        }
        return SS$_NORMAL;
    }

    /* ASCII counted string - !AD */
    if (strcmp(directive, "AD") == 0) {
        uint32_t len = (uint32_t)(**args);
        (*args)++;
        char *str = (char *)(uintptr_t)(**args);
        (*args)++;
        if (str && len > 0) {
            if (append_string(out, out_end, str, len))
                return SS$_BUFFEROVF;
        }
        return SS$_NORMAL;
    }

    /* Numeric formats */
    char format_buf[128];
    int64_t val = 0;
    int is_numeric = 1;
    int base = 10;
    int width = 0;
    char pad = ' ';
    int is_signed = 0;

    if (strcmp(directive, "SL") == 0) {
        val = (int32_t)(**args); (*args)++; is_signed = 1;
    } else if (strcmp(directive, "UL") == 0) {
        val = (uint32_t)(**args); (*args)++;
    } else if (strcmp(directive, "SW") == 0) {
        val = (int16_t)(uint16_t)(**args); (*args)++; is_signed = 1;
    } else if (strcmp(directive, "UW") == 0) {
        val = (uint16_t)(**args); (*args)++;
    } else if (strcmp(directive, "SB") == 0) {
        val = (int8_t)(uint8_t)(**args); (*args)++; is_signed = 1;
    } else if (strcmp(directive, "UB") == 0) {
        val = (uint8_t)(**args); (*args)++;
    } else if (strcmp(directive, "XL") == 0) {
        val = (uint32_t)(**args); (*args)++; base = 16;
    } else if (strcmp(directive, "XW") == 0) {
        val = (uint16_t)(**args); (*args)++; base = 16;
    } else if (strcmp(directive, "XB") == 0) {
        val = (uint8_t)(**args); (*args)++; base = 16;
    } else if (strcmp(directive, "OL") == 0) {
        val = (uint32_t)(**args); (*args)++; base = 8;
    } else if (strcmp(directive, "OW") == 0) {
        val = (uint16_t)(**args); (*args)++; base = 8;
    } else if (strcmp(directive, "OB") == 0) {
        val = (uint8_t)(**args); (*args)++; base = 8;
    } else if (strcmp(directive, "ZL") == 0) {
        val = (uint32_t)(**args); (*args)++; width = 8; pad = '0';
    } else if (strcmp(directive, "ZW") == 0) {
        val = (uint16_t)(**args); (*args)++; width = 4; pad = '0';
    } else if (strcmp(directive, "ZB") == 0) {
        val = (uint8_t)(**args); (*args)++; width = 3; pad = '0';
    } else {
        is_numeric = 0;
    }

    if (is_numeric) {
        *last_numeric_arg = val;
        /* For signed formats, pass the value directly as it's already sign-extended */
        int len = format_integer(format_buf, sizeof(format_buf), val, base, width, pad);
        if (len < 0 || append_string(out, out_end, format_buf, len))
            return SS$_BUFFEROVF;
        return SS$_NORMAL;
    }

    /* Unsupported or unrecognized directive - skip it */
    return SS$_NORMAL;
}

/*
 * sys$faol - Formatted ASCII output with argument list
 *
 * Implementation of the VMS SYS$FAOL system service.
 * Processes a FAO control string and argument list, producing
 * formatted output into the specified buffer.
 */
uint32_t sys$faol(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    const uint64_t *prmlst)
{
    if (!ctrstr || !outbuf) return SS$_BADPARAM;
    if (!ctrstr->dsc$a_pointer || ctrstr->dsc$w_length == 0) return SS$_BADPARAM;
    if (!outbuf->dsc$a_pointer) return SS$_BADPARAM;

    /* Working buffer for output */
    char temp_buf[FAO_MAX_INTERNAL_BUF];
    char *out = temp_buf;
    char *out_end = temp_buf + sizeof(temp_buf);

    const char *ctrl = ctrstr->dsc$a_pointer;
    const char *ctrl_end = ctrl + ctrstr->dsc$w_length;
    const uint64_t *args = prmlst;

    uint32_t status = SS$_NORMAL;
    int64_t last_numeric_arg = 0;

    /* Process control string */
    while (ctrl < ctrl_end) {
        if (*ctrl == '!') {
            /* FAO directive */
            uint32_t dir_status = process_directive(&ctrl, ctrl_end, &out, &out_end, &args, &last_numeric_arg);
            if (dir_status != SS$_NORMAL) {
                status = dir_status;
                break;  /* Stop on any error */
            }
        } else {
            /* Literal character */
            if (append_char(&out, &out_end, *ctrl)) {
                status = SS$_BUFFEROVF;
                break;
            }
            ctrl++;
        }
    }

    /* Calculate output length */
    size_t output_len = out - temp_buf;
    if (outlen) {
        *outlen = (uint16_t)(output_len > 65535 ? 65535 : output_len);
    }

    /* Copy to output descriptor */
    if (outbuf->dsc$b_class == DSC$K_CLASS_D) {
        /* Dynamic descriptor - reallocate if needed */
        if (outbuf->dsc$w_length < output_len) {
            /* Pass pointer directly: if realloc fails, it returns NULL and does
             * NOT free the original buffer.  outbuf->dsc$a_pointer still holds
             * the old (valid) pointer, so the descriptor remains consistent on
             * the SS$_INSFMEM return path. */
            char *new_buf = (char *)realloc(outbuf->dsc$a_pointer, output_len);
            if (!new_buf) return SS$_INSFMEM;
            outbuf->dsc$a_pointer = new_buf;
            outbuf->dsc$w_length = (uint16_t)(output_len > 65535 ? 65535 : output_len);
        }
        memcpy(outbuf->dsc$a_pointer, temp_buf,
               output_len < outbuf->dsc$w_length ? output_len : outbuf->dsc$w_length);
    } else {
        /* Static descriptor - truncate if needed */
        size_t copy_len = output_len;
        if (copy_len > outbuf->dsc$w_length) {
            copy_len = outbuf->dsc$w_length;
            status = SS$_BUFFEROVF;
        }
        memcpy(outbuf->dsc$a_pointer, temp_buf, copy_len);
    }

    return status;
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
    /* Build argument array from varargs */
    uint64_t args[256];
    int arg_count = 0;

    va_list ap;
    va_start(ap, outbuf);

    /* Extract up to 256 arguments */
    while (arg_count < 256) {
        args[arg_count++] = va_arg(ap, uint64_t);

        /* Heuristic: stop if we've processed the control string length worth of args
         * In practice, most FAO calls have fewer than 20 arguments */
        if (arg_count >= 64) break;
    }

    va_end(ap);

    /* Call sys$faol with the argument array */
    return sys$faol(ctrstr, outlen, outbuf, args);
}
