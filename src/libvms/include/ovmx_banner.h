/*
 * ovmx_banner.h - SYS$ANNOUNCE / SYS$WELCOME banner resolution (INV-1)
 *
 * On OpenVMS the login banners are NOT compiled into LOGINOUT -- they are
 * system logical names a manager defines at boot (SYS$MANAGER:SYLOGICALS.COM,
 * OVMX: SYS$MANAGER:SYLOGICALS.CONF):
 *
 *   SYS$ANNOUNCE  displayed BEFORE the "Username:" prompt. Undefined by
 *                 default -- nothing is printed.
 *   SYS$WELCOME   displayed AFTER successful authentication. Undefined by
 *                 default -- LOGINOUT prints its built-in welcome.
 *
 * In both cases, if the equivalence string begins with '@', the remainder is
 * a file specification and the FILE'S CONTENTS are displayed (this is how a
 * site gets a multi-line banner); otherwise the string itself is displayed.
 *
 * Both are deliberately left UNDEFINED by lnm_setup_defaults(), exactly as
 * real VMS ships them: a sysadmin typing
 *     $ DEFINE/SYSTEM SYS$WELCOME "..."
 * must see the banner change, and must see the built-in return when they
 * deassign it. Hardcoding a printf() defeats that -- it was the pre-INV-1
 * behavior and it is a first-ten-minutes tell.
 *
 * The built-in fallback text is owned by the identity SSOT (ovmx_identity.h),
 * so there is still exactly one place that knows what OVMX calls itself.
 *
 * Spec: docs/design-authenticity-roadmap.md sec 4.5 INV-1 (rd vms-e652).
 * Oracle: OpenVMS System Manager's Manual (SYS$ANNOUNCE / SYS$WELCOME).
 *
 * HEADER-ONLY BY DESIGN. libvms is built BEFORE vmslnm in the library
 * dependency graph, so a libvms .c file cannot call lnm_translate(). Keeping
 * this inline means only the translation units that actually display banners
 * take the vmslnm/vmsfs dependency. Consumers must link vmslnm and vmsfs.
 */
#ifndef OVMX_BANNER_H
#define OVMX_BANNER_H

#include <stdio.h>
#include <string.h>

#include "ssdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"
#include "ovmx_identity.h"

/* Longest banner equivalence string we will translate. */
#define OVMX_BANNER_MAXLEN  1024

/*
 * ovmx_banner_display_file - Display the contents of a VMS file spec.
 * Silently does nothing if the file cannot be opened -- a mistyped
 * SYS$WELCOME must never wedge or spew errno text into a login session
 * (INV-4: no Linux leak on a VMS-facing surface).
 */
static inline void ovmx_banner_display_file(const char *vms_spec, FILE *out)
{
    char linux_path[1024];

    /* VMS status convention: odd = success. vmsfs_to_linux_path returns
     * SS$_NORMAL, not 0. */
    if (!$VMS_STATUS_SUCCESS(vmsfs_to_linux_path(vms_spec, linux_path,
                                                 sizeof(linux_path))))
        return;

    FILE *f = fopen(linux_path, "r");
    if (!f)
        return;

    char line[OVMX_BANNER_MAXLEN];
    while (fgets(line, sizeof(line), f))
        fputs(line, out);

    fclose(f);
}

/*
 * ovmx_banner_display - Display the banner named by a logical name.
 *
 * @logical_name: "SYS$ANNOUNCE" or "SYS$WELCOME"
 * @fallback:     text to display when the logical is undefined; NULL means
 *                display nothing (the correct SYS$ANNOUNCE default)
 * @out:          stream to write to
 *
 * Searches LNM$FILE_DEV (process -> job -> group -> system) so a process- or
 * job-level definition overrides the system one, as on VMS.
 */
static inline void ovmx_banner_display(const char *logical_name,
                                       const char *fallback, FILE *out)
{
    if (!logical_name || !out)
        return;

    char equiv[OVMX_BANNER_MAXLEN];
    equiv[0] = '\0';

    uint32_t st = lnm_translate(lnm_get_manager(), LNM_FILE_DEV, logical_name,
                                equiv, sizeof(equiv), NULL, NULL);

    if (st == SS$_NORMAL && equiv[0] != '\0') {
        if (equiv[0] == '@') {
            /* "@filespec" -- display the file's contents (multi-line site
             * banner). Skip the '@' and any spaces after it. */
            const char *spec = equiv + 1;
            while (*spec == ' ' || *spec == '\t')
                spec++;
            if (*spec != '\0')
                ovmx_banner_display_file(spec, out);
        } else {
            fprintf(out, "%s\n", equiv);
        }
        return;
    }

    /* Logical undefined -- built-in default, if this banner has one. */
    if (fallback)
        fprintf(out, "%s\n", fallback);
}

/*
 * ovmx_banner_announce - SYS$ANNOUNCE, before the Username: prompt.
 * No built-in default: silence is the VMS default.
 */
static inline void ovmx_banner_announce(FILE *out)
{
    ovmx_banner_display("SYS$ANNOUNCE", NULL, out);
}

/*
 * ovmx_banner_welcome - SYS$WELCOME, after successful authentication.
 * Falls back to the badged OVMX identity (INV-0).
 */
static inline void ovmx_banner_welcome(FILE *out)
{
    char def[OVMX_IDENTITY_MAXLEN + 32];
    snprintf(def, sizeof(def), "   Welcome to %s", ovmx_product_banner());
    ovmx_banner_display("SYS$WELCOME", def, out);
}

#endif /* OVMX_BANNER_H */
