/*
 * rightslist.c - the rights database reader (vms-2f8)
 *
 * See rightslist.h for the interface and the two-sources rationale, and
 * docs/oracle/vax73-rights-database.md for every value this file resolves,
 * measured on OpenVMS VAX V7.3.
 *
 * WHAT THIS REPLACES. src/vmsdcl/dcl_lexical.c's lex_identifier() held two
 * hardcoded branches -- SYSTEM and DEFAULT -- and answered the miss for
 * everything else. Both values were correct (pinned in an earlier round) and
 * both were still fabrications in the sense CLAUDE.md Rule 11 means: the
 * command produced the answer itself instead of reading the facility that
 * owns it. LOCAL, BATCH, NETWORK, REMOTE, INTERACTIVE and DIALUP were simply
 * absent, and OVMX's own SYSUAF accounts (GUEST, USER1, USER2, OPERATOR) had
 * no identifier at all.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "rightslist.h"
#include "sysuaf.h"
#include "str_util.h"
#include "vmsfs/filespec.h"
#include "rms_textfile.h"

/* ------------------------------------------------------------------ */
/* Value notation                                                      */
/* ------------------------------------------------------------------ */

/*
 * Parse the VALUE column of a RIGHTSLIST.DAT row.
 *
 * Two notations, and they are the two AUTHORIZE prints -- a row in the file
 * reads the same as the row the tool shows (oracle §4):
 *
 *     %Xnnnnnnnn   general identifier, hexadecimal
 *     [g,m]        UIC identifier, OCTAL in both fields
 *
 * THE BASE IS NOT A STYLE CHOICE. vms-e60 established that every VMS UIC is
 * written in octal, and that reading one as decimal produced two different
 * UICs for one account. A rights-database row is a UIC in exactly the same
 * sense, so it is read in exactly the same base. Anything else -- a bare
 * decimal number, a bare hex number -- is rejected rather than guessed at,
 * because a value silently misread here becomes a wrong identifier rather
 * than a visible error.
 *
 * Returns 0 and writes *value on success, -1 on a malformed value.
 */
static int parse_value(const char *s, uint32_t *value)
{
    while (*s == ' ' || *s == '\t')
        s++;

    if ((s[0] == '%') && (s[1] == 'X' || s[1] == 'x')) {
        char *end = NULL;
        unsigned long v = strtoul(s + 2, &end, 16);
        if (end == s + 2)
            return -1;
        while (*end == ' ' || *end == '\t')
            end++;
        if (*end != '\0')
            return -1;
        *value = (uint32_t)v;
        return 0;
    }

    if (s[0] == '[') {
        char *end = NULL;
        unsigned long g = strtoul(s + 1, &end, 8);   /* OCTAL -- vms-e60 */
        if (end == s + 1 || *end != ',')
            return -1;
        const char *m_start = end + 1;
        unsigned long m = strtoul(m_start, &end, 8); /* OCTAL -- vms-e60 */
        if (end == m_start || *end != ']')
            return -1;
        if (g > 0xFFFF || m > 0xFFFF)
            return -1;
        *value = (uint32_t)((g << 16) | m);
        return 0;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* RIGHTSLIST.DAT scan                                                 */
/* ------------------------------------------------------------------ */

/*
 * Walk RIGHTSLIST.DAT once, matching either by name (case-insensitive) or by
 * value. 'want_name' NULL means match on 'want_value'.
 *
 * ONE SCANNER FOR BOTH DIRECTIONS, for the same reason sysuaf_scan() is one
 * scanner: a second copy of parse_value() is a second base decision waiting
 * to disagree with the first.
 *
 * Returns 0 on a match, writing *out_value and/or out_name; -1 otherwise,
 * INCLUDING when the file is absent. An absent rights database is a miss,
 * not a reason to fall back to a built-in table (CLAUDE.md Rule 9: fail
 * honestly, never fake). The caller renders that miss as VMS renders it.
 */
static int rightslist_scan(const char *want_name, uint32_t want_value,
                           uint32_t *out_value, char *out_name, size_t bufsz)
{
    /*
     * The rights database is read the VMS way: RMS $OPEN/$GET over the
     * Files-11 ODS-2 ACP (vms-274), not fopen on a /vms passthrough. An absent
     * or unreachable database is a miss, never a fall-back to a built-in table
     * (Rule 9 / INV-6) -- rms_textfile_open() returns NULL and the caller
     * renders the miss as VMS renders it. */
    rms_textfile_t *fp = rms_textfile_open(RIGHTSLIST_PATH);
    if (!fp)
        return -1;

    char search[RIGHTSLIST_NAME_MAX];
    search[0] = '\0';
    if (want_name) {
        strncpy(search, want_name, sizeof(search) - 1);
        search[sizeof(search) - 1] = '\0';
        str_upcase(search);
    }

    char line[512];
    int found = -1;
    while (found != 0 && rms_textfile_getline(fp, line, sizeof(line), NULL)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        str_trim(line);
        if (line[0] == '\0')
            continue;

        /* IDENTIFIER:VALUE:ATTRIBUTES -- ATTRIBUTES is present but unused
         * here; the reader has no consumer for KGB$M_ bits yet and every
         * shipped row's column is empty because it is empty on the oracle. */
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *value_str = colon + 1;
        char *colon2 = strchr(value_str, ':');
        if (colon2)
            *colon2 = '\0';

        char name[RIGHTSLIST_NAME_MAX];
        strncpy(name, line, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        str_trim(name);
        str_upcase(name);
        if (name[0] == '\0')
            continue;

        uint32_t value;
        if (parse_value(value_str, &value) != 0)
            continue;   /* malformed row: not an identifier, not an answer */

        if (want_name ? (strcmp(name, search) == 0) : (value == want_value)) {
            if (out_value)
                *out_value = value;
            if (out_name && bufsz)
                snprintf(out_name, bufsz, "%s", name);
            found = 0;
        }
    }

    rms_textfile_close(fp);
    return found;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

int rightslist_name_to_value(const char *name, uint32_t *value)
{
    if (!name || !*name || !value)
        return -1;

    /* General identifiers first: RIGHTSLIST.DAT is the rights database
     * proper. Order does not matter for correctness -- a name cannot be
     * both a general identifier and an account -- but it puts the file
     * this item exists to start reading on the primary path. */
    if (rightslist_scan(name, 0, value, NULL, 0) == 0)
        return 0;

    /* UIC identifiers, derived from SYSUAF so an account has exactly one
     * UIC in the whole system (rightslist.h). */
    sysuaf_record_t rec;
    if (sysuaf_lookup(name, &rec) == 0) {
        *value = (rec.uic_group << 16) | rec.uic_member;
        return 0;
    }

    return -1;
}

int rightslist_value_to_name(uint32_t value, char *buf, size_t bufsz)
{
    if (!buf || !bufsz)
        return -1;

    if (rightslist_scan(NULL, value, NULL, buf, bufsz) == 0)
        return 0;

    sysuaf_record_t rec;
    if (sysuaf_lookup_by_uic(value, &rec) == 0) {
        snprintf(buf, bufsz, "%s", rec.username);
        return 0;
    }

    return -1;
}
