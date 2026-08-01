/*
 * sysuaf.c - SYSUAF (System User Authorization File) shared library
 *
 * Extracted from tools/vms_login.c and tools/vms_ssh_auth.c so that
 * the upcoming vmssshd (and any other consumer) can reuse the same
 * lookup and authentication logic without duplicating code.
 *
 * No VMS runtime initialization is required — this is pure file I/O
 * and SHA256 hashing.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "sha256.h"
#include "str_util.h"
#include "sysuaf.h"
#include "vmsfs/filespec.h"
#include "vms/privs.h"

/* str_upcase() and str_trim() replaced by str_str_upcase()/str_trim() from str_util.h */

/* ------------------------------------------------------------------ */
/* sysuaf_lookup                                                       */
/* ------------------------------------------------------------------ */

/*
 * Parse ONE SYSUAF.DAT line into *rec, in place (the line buffer is
 * modified). Returns 0 on success, -1 for a comment, a blank, or a
 * malformed row.
 *
 * Factored out of sysuaf_lookup() when sysuaf_lookup_by_uic() was added
 * (vms-cb5), so the two searches cannot drift into disagreeing about
 * what a row means -- a UIC->name lookup that parsed the file
 * differently from the name->UIC lookup would be a way for the same
 * account to have two identities.
 */
static int sysuaf_parse_line(char *line, sysuaf_record_t *rec)
{
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        return -1;

    str_trim(line);

    /* Parse: USERNAME|PASSWORD_HASH|UIC_GROUP|UIC_MEMBER|DEFAULT_DIR|FLAGS|PRIVILEGES
     * Pipe delimiter avoids conflict with VMS device colons in DEFAULT_DIR. */
    char *fields[7];
    char *p = line;
    int nf = 0;

    for (nf = 0; nf < 7 && p; nf++) {
        fields[nf] = p;
        char *sep = strchr(p, '|');
        if (sep) {
            *sep = '\0';
            p = sep + 1;
        } else {
            p = NULL;
        }
    }

    if (nf < 5)
        return -1;  /* malformed line */

    memset(rec, 0, sizeof(*rec));
    strncpy(rec->username, fields[0], sizeof(rec->username) - 1);
    str_upcase(rec->username);
    strncpy(rec->password_hash, fields[1], sizeof(rec->password_hash) - 1);
    rec->uic_group  = (uint32_t)strtoul(fields[2], NULL, 10);
    rec->uic_member = (uint32_t)strtoul(fields[3], NULL, 10);
    strncpy(rec->default_dir, fields[4], sizeof(rec->default_dir) - 1);
    if (nf > 5)
        strncpy(rec->flags, fields[5], sizeof(rec->flags) - 1);
    if (nf > 6)
        strncpy(rec->privileges, fields[6], sizeof(rec->privileges) - 1);
    return 0;
}

/*
 * Look up a user in SYSUAF_PATH.
 * The username comparison is case-insensitive.
 * On success the record is written to *rec and 0 is returned.
 * Returns -1 if the file cannot be opened or the user is not found.
 */
int sysuaf_lookup(const char *username, sysuaf_record_t *rec)
{
    char sysuaf_linux[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux, sizeof(sysuaf_linux));
    FILE *fp = fopen(sysuaf_linux, "r");
    if (!fp)
        return -1;

    /* Build uppercased search key */
    char search_copy[64];
    strncpy(search_copy, username, sizeof(search_copy) - 1);
    search_copy[sizeof(search_copy) - 1] = '\0';
    str_upcase(search_copy);

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        sysuaf_record_t cand;
        if (sysuaf_parse_line(line, &cand) != 0)
            continue;
        /* cand.username is already upcased by sysuaf_parse_line(). */
        if (strcmp(cand.username, search_copy) == 0) {
            *rec = cand;
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;  /* User not found */
}

/* ------------------------------------------------------------------ */
/* sysuaf_lookup_by_uic                                                */
/* ------------------------------------------------------------------ */

/*
 * Reverse of sysuaf_lookup(): find the account that owns a UIC.
 * See the header comment on the declaration in sysuaf.h for why this
 * exists (vms-cb5 / vms-f39) -- it replaces a getpwuid() call that let
 * the HOST's /etc/passwd answer a VMS question.
 */
int sysuaf_lookup_by_uic(uint32_t uic_group, uint32_t uic_member,
                         sysuaf_record_t *rec)
{
    char sysuaf_linux[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux, sizeof(sysuaf_linux));
    FILE *fp = fopen(sysuaf_linux, "r");
    if (!fp)
        return -1;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        sysuaf_record_t cand;
        if (sysuaf_parse_line(line, &cand) != 0)
            continue;
        if (cand.uic_group == uic_group && cand.uic_member == uic_member) {
            *rec = cand;
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;  /* No account owns that UIC */
}

/* ------------------------------------------------------------------ */
/* sysuaf_authenticate                                                 */
/* ------------------------------------------------------------------ */

/*
 * Check that 'password' matches the stored hash in 'rec'.
 *
 * An empty/unset hash NEVER authenticates (vms-08f). This was previously
 * "empty hash = no password required" -- an AUTH BYPASS, not a feature: it
 * let any string typed at the Password: prompt succeed for every account
 * whose SYSUAF row had not yet been given a real hash. Measured directly
 * against a real QEMU boot before the fix (vms-72c): SYSTEM + a
 * deliberately wrong password reached a DCL prompt. vms-08f found the same
 * bypass still live for OPERATOR/DEFAULT/USER1/USER2 -- vms-72c had closed
 * it only for the two accounts its own UAT drove (SYSTEM/GUEST).
 *
 * RULE 10 DISPOSITION (two legal answers, never three): does OpenVMS have
 * a state where an account authenticates with no password check at all?
 * Yes, but it is an EXPLICIT, DECLARED per-account flag, not the default
 * meaning of an absent password field:
 *
 *   - UAI$M_AUTOLOGIN (uaidef.h) -- OpenVMS Guide to System Security,
 *     "Automatic Login Accounts": "To protect automatic login accounts,
 *     set the AUTOLOGIN flag in the account's UAF record. This flag makes
 *     the account available only by autologin, batch, and network proxy."
 *     (https://www0.mi.infn.it/~calcolo/OpenVMS/ssb71/6015/6017p017.htm)
 *   - Same manual, S7.3.1 "Types of Passwords": "With the exception of an
 *     automatic login account, all users must have at least one password
 *     to log in."
 *     (https://www0.mi.infn.it/~calcolo/OpenVMS/ssb71/6346/6346p011.htm)
 *
 * So a SYSUAF row with no password on file is not a state OpenVMS treats
 * as "no password required" -- it is not a state OpenVMS reaches at all:
 * the only account that can skip the password prompt is one explicitly
 * flagged AUTOLOGIN (and even then only from its bound terminal/batch/
 * proxy path, which OVMX does not implement). This is candidate (a):
 * MATCH VMS by making "no hash on file" mean the account cannot
 * authenticate at all. It is also the HIDE
 * IT half of Rule 10 for OVMX specifically -- until AUTOLOGIN gating
 * exists, "authenticate with an unset hash" is not a condition this
 * function may handle; it must be unreachable, so every unset-hash
 * account refuses every password, with no exception carved out here.
 *
 * A non-empty hash is compared as a SHA256 hex string (case-insensitive).
 * Returns 1 on match, 0 on mismatch (including every unset-hash case).
 */
int sysuaf_authenticate(const sysuaf_record_t *rec, const char *password)
{
    /* An unset hash can never authenticate -- see the Rule 10 note above. */
    if (rec->password_hash[0] == '\0')
        return 0;

    /* Hash the supplied password with SHA256 */
    char hex[65];
    sha256_hex((const uint8_t *)password, strlen(password), hex);

    /* Compare against stored hex hash (case-insensitive) */
    return (strcasecmp(hex, rec->password_hash) == 0);
}

/* ------------------------------------------------------------------ */
/* sysuaf_parse_privileges                                             */
/* ------------------------------------------------------------------ */

/*
 * Thin wrapper around parse_privilege_string() from vms/privs.h.
 * Converts a comma-separated privilege string (e.g. "TMPMBX,NETMBX,OPER")
 * into a uint64_t bitmask.
 */
uint64_t sysuaf_parse_privileges(const char *priv_string)
{
    return parse_privilege_string(priv_string);
}
