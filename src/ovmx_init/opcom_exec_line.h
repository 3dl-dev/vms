/*
 * opcom_exec_line.h - recognize an EXECUTIVE operator line in the substrate's
 * kernel log (rd vms-c56).
 *
 * The executive composes its operator lines in VMS form already --
 * "%CNXMAN, ...", "%PEA0, ...", "%DLM, ...", "%MSCP_CL, ..." -- and since rd
 * vms-c56 only the few a VMS console prints go to OPA0:; the rest are
 * diagnostics that reach OPERATOR.LOG through the kernel-log bridge (Linux
 * /dev/kmsg, opcom_kmsg.c; NetBSD /dev/klog, ovmx_boot_netbsd.c). Such a line
 * already wears its facility, so the bridge files it AS IT IS -- it is not a
 * Linux/NetBSD kernel message, and re-styling it as "%SYSKRNL-I-KERNEL, %CNXMAN,
 * ..." would misname its origin.
 *
 * What the kernel log adds around the text, and this skips:
 *   - Linux /dev/kmsg escapes control characters as "\xNN": an executive
 *     console line starts on a fresh line, so its record text begins "\x0a";
 *   - NetBSD's message buffer stamps a line "[    83.2700000] ".
 *
 * Returns a pointer to the "%FACILITY..." text inside `text`, or NULL when the
 * line is not an executive operator line. Pure (no libc), shared
 * by both bridges, unit-tested in tests/ovmx_init/test_opcom_kmsg.c.
 */
#ifndef OVMX_OPCOM_EXEC_LINE_H
#define OVMX_OPCOM_EXEC_LINE_H

static inline int opcom_exec_is_fac_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '$' || c == '_';
}

static inline const char *opcom_exec_line(const char *text)
{
    const char *p = text;
    const char *f;

    if (p == 0)
        return 0;
    for (;;) {
        if (p[0] == '\\' && p[1] == 'x' && p[2] == '0' &&
            (p[3] == 'a' || p[3] == 'd' || p[3] == 'A' || p[3] == 'D'))
            p += 4;
        else if (*p == '\n' || *p == '\r' || *p == ' ')
            p++;
        else
            break;
    }
    if (*p == '[') {                       /* a substrate uptime stamp */
        const char *q = p + 1;

        while (*q == ' ' || (*q >= '0' && *q <= '9') || *q == '.')
            q++;
        if (*q != ']' || q == p + 1)
            return 0;
        p = q + 1;
        while (*p == ' ')
            p++;
    }
    if (p[0] != '%' || !((p[1] >= 'A' && p[1] <= 'Z')))
        return 0;
    f = p + 1;
    while (opcom_exec_is_fac_char(*f))
        f++;
    if (*f == ',' || *f == '-')
        return p;
    return 0;
}

#endif /* OVMX_OPCOM_EXEC_LINE_H */
