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

    /* Working buffer for output — heap-allocated to avoid 64KB stack frame */
    char *temp_buf = (char *)malloc(FAO_MAX_INTERNAL_BUF);
    if (!temp_buf) return SS$_INSFMEM;
    char *out = temp_buf;
    char *out_end = temp_buf + FAO_MAX_INTERNAL_BUF;

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
            if (!new_buf) { free(temp_buf); return SS$_INSFMEM; }
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

    free(temp_buf);
    return status;
}

/*
 * count_fao_args - Count the number of arguments consumed by FAO directives
 *
 * Scans the control string for FAO directives and returns how many
 * uint64_t arguments they will consume from the parameter list.
 */
/*
 * Also exported as sys$fao_count_args for use by lib$sys_fao.
 */
int count_fao_args(const char *ctrl, uint16_t len) {
    const char *end = ctrl + len;
    int count = 0;

    while (ctrl < end) {
        if (*ctrl != '!') { ctrl++; continue; }
        ctrl++;  /* skip '!' */
        if (ctrl >= end) break;

        /* Skip numeric prefix */
        int has_repeat = 0;
        while (ctrl < end && *ctrl >= '0' && *ctrl <= '9') {
            has_repeat = 1;
            ctrl++;
        }
        if (ctrl >= end) break;

        char first = *ctrl;

        /* Single-char directives with no args */
        if (first == '/' || first == '_' || first == '!') {
            ctrl++;
            continue;
        }

        /* !n*c or !*c — consumes 1 arg if no numeric prefix */
        if (first == '*') {
            ctrl++;
            if (ctrl < end) ctrl++;  /* skip the fill char */
            if (!has_repeat) count++;
            continue;
        }

        /* %S — no args */
        if (first == '%') {
            ctrl++;
            if (ctrl < end && (*ctrl == 'S' || *ctrl == 's')) ctrl++;
            continue;
        }

        /* Parse alphabetic directive */
        char dir[4] = {0};
        int dlen = 0;
        while (ctrl < end && dlen < 3 && ((*ctrl >= 'A' && *ctrl <= 'Z') ||
               (*ctrl >= 'a' && *ctrl <= 'z'))) {
            dir[dlen++] = (*ctrl >= 'a') ? (*ctrl - 32) : *ctrl;
            ctrl++;
        }

        /* AD consumes 2 args (length + pointer), all others consume 1 */
        if (dlen == 2 && dir[0] == 'A' && dir[1] == 'D')
            count += 2;
        else if (dlen > 0)
            count += 1;
    }
    return count;
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
