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
/* sysuaf_lookup / sysuaf_lookup_by_uic                                */
/* ------------------------------------------------------------------ */

/*
 * ONE SCANNER, TWO LOOKUP DIRECTIONS (vms-2f8).
 *
 * sysuaf_lookup() answers by name; sysuaf_lookup_by_uic() answers by UIC,
 * which is what F$IDENTIFIER's NUMBER_TO_NAME direction needs now that it
 * reads the rights database instead of a hardcoded pair. Both walk the same
 * loop below and share one field parser, so there is exactly one place that
 * decides how a SYSUAF row becomes a record -- in particular exactly one
 * place that reads the UIC fields in the base vms-e60 pinned. A second copy
 * of that parse would be a second answer waiting to drift.
 *
 * 'want_uic' is matched only when 'username' is NULL.
 * On success the record is written to *rec and 0 is returned.
 * Returns -1 if the file cannot be opened or nothing matches.
 */
static int sysuaf_scan(const char *username, uint32_t want_uic,
                       sysuaf_record_t *out)
{
    char sysuaf_linux[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux, sizeof(sysuaf_linux));
    FILE *fp = fopen(sysuaf_linux, "r");
    if (!fp)
        return -1;

    /* Build uppercased search key */
    char search_copy[64];
    search_copy[0] = '\0';
    if (username) {
        strncpy(search_copy, username, sizeof(search_copy) - 1);
        search_copy[sizeof(search_copy) - 1] = '\0';
        str_upcase(search_copy);
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

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
            continue;  /* malformed line */

        /* Every row is parsed into 'row' before either direction decides
         * whether it matched, so the UIC direction reads the SAME parse the
         * name direction does rather than a second copy of it. */
        {
            sysuaf_record_t row;
            memset(&row, 0, sizeof(row));
            /* 'rec' is an alias for the row being parsed, kept so the field
             * fill below -- including the vms-e60 octal block, which is the
             * one thing in this file nobody should be re-typing -- stays
             * byte-identical to what it was before this scan grew a second
             * caller. The function's out-parameter is 'out'. */
            sysuaf_record_t *rec = &row;
            strncpy(rec->username, fields[0], sizeof(rec->username) - 1);
            str_upcase(rec->username);
            strncpy(rec->password_hash, fields[1], sizeof(rec->password_hash) - 1);
            /*
             * ============================================================
             * SYSUAF.DAT's UIC FIELDS ARE OCTAL (vms-e60)
             * ============================================================
             * These were strtoul(..., 10). That is the bug, and which base
             * is right was DERIVED from the oracle rather than picked:
             *
             *   ORACLE (measured twice on lab nodes vax3/vax2, and asserted
             *   in-tree at tests/qemu/test_syssvc_ident.c):
             *       F$IDENTIFIER("DEFAULT","NAME_TO_NUMBER") -> 8388736
             *       = %X00800080 -> group 0x80 = 128, member 0x80 = 128,
             *       which is UIC [200,200] written the way VMS writes UICs.
             *
             *   SYSUAF.DAT ships 'DEFAULT||200|200|...'. Read as OCTAL that
             *   is 128/128 and reproduces the oracle exactly. Read as
             *   DECIMAL it is 200/200 -> 13107400, which matches nothing.
             *
             * SYSTEM's 1|4 reads identically in both bases -- that is the
             * coincidence that hid this for as long as SYSTEM was the only
             * account anyone logged in as. Any account whose UIC digits
             * differ between bases showed two different UICs for one
             * account: LOGINOUT stamped one value into the executive while
             * F$IDENTIFIER answered another.
             *
             * VMS's own convention is quoted in this tree at
             * src/libvms/include/ovmx_secparam.h and docs/oracle/
             * vax73-privileges.md: "bear in mind that numbers in a UIC are
             * octal". So the display, the write path and the /UIC=[g,m]
             * command parse in tools/vms_authorize.c are octal too; all
             * nine sites move together, because a partial change just
             * relocates the disagreement (Rule 10 -- one answer, pinned).
             * ============================================================
             */
            rec->uic_group  = (uint32_t)strtoul(fields[2], NULL, 8);
            rec->uic_member = (uint32_t)strtoul(fields[3], NULL, 8);
            strncpy(rec->default_dir, fields[4], sizeof(rec->default_dir) - 1);
            if (nf > 5)
                strncpy(rec->flags, fields[5], sizeof(rec->flags) - 1);
            if (nf > 6)
                strncpy(rec->privileges, fields[6], sizeof(rec->privileges) - 1);

            if (username
                ? strcmp(row.username, search_copy) == 0
                : ((row.uic_group << 16) | row.uic_member) == want_uic) {
                *out = row;
                fclose(fp);
                return 0;
            }
        }
    }

    fclose(fp);
    return -1;  /* No matching account */
}

/*
 * Look up a user in SYSUAF_PATH.
 * The username comparison is case-insensitive.
 * On success the record is written to *rec and 0 is returned.
 * Returns -1 if the file cannot be opened or the user is not found.
 */
int sysuaf_lookup(const char *username, sysuaf_record_t *rec)
{
    if (!username || !rec)
        return -1;
    return sysuaf_scan(username, 0, rec);
}

/*
 * Look up the account whose UIC is 'uic' ((group << 16) | member).
 *
 * Added for vms-2f8: on VMS every UAF account has a matching UIC identifier
 * in the rights database, so F$IDENTIFIER's NUMBER_TO_NAME direction has to
 * be able to go from a UIC back to the account name. Deriving that from
 * SYSUAF rather than duplicating the UIC rows into RIGHTSLIST.DAT keeps one
 * source for an account's UIC (see the shipped RIGHTSLIST.DAT header).
 *
 * FIRST MATCH WINS, and OVMX's shipped SYSUAF has no duplicate UICs. VMS
 * treats a UIC as identifying one account; if a site ever writes two rows
 * with one UIC that is a malformed SYSUAF, not a case with a defined answer,
 * and nothing here invents one.
 *
 * Returns 0 on success, -1 if the file cannot be opened or no account holds
 * that UIC.
 */
int sysuaf_lookup_by_uic(uint32_t uic, sysuaf_record_t *rec)
{
    if (!rec)
        return -1;
    return sysuaf_scan(NULL, uic, rec);
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
