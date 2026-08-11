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
#include <unistd.h>

#include "sha256.h"
#include "str_util.h"
#include "sysuaf.h"
#include "vmsfs/filespec.h"
#include "vms/privs.h"

/* str_upcase() and str_trim() replaced by str_str_upcase()/str_trim() from str_util.h */

#include "uaidef.h"

/* ------------------------------------------------------------------ */
/* THE FORMAT: one reader, one writer (vms-9b7)                        */
/* ------------------------------------------------------------------ */

/*
 * Read one line, REPORTING truncation rather than hiding it.
 *
 * THIS IS THE FUNCTION THE BUG WAS THE ABSENCE OF. Every one of the five
 * parsers this file replaces called fgets() straight into a fixed buffer and
 * used whatever came back. fgets() on an over-length line returns a PREFIX
 * that is indistinguishable from a complete short line -- so a 512-byte
 * reader turned a valid seven-field SYSTEM row into a five-field one, the
 * privileges field ceased to exist, and PID 1 halted the boot reporting that
 * SYSUAF had no SYSTEM record at all. Nothing warned, at any of the five
 * sites.
 *
 * The remainder of an over-length line is DRAINED here, so the caller's next
 * read begins at a real record boundary. Leaving it in the stream is how the
 * tail of one row gets presented to the caller as a row of its own.
 */
int sysuaf_read_line(FILE *fp, char *buf, size_t bufsz, int *too_long)
{
    if (too_long)
        *too_long = 0;
    if (!fp || !buf || bufsz < 2)
        return 0;

    if (!fgets(buf, (int)bufsz, fp))
        return 0;

    size_t len = strlen(buf);
    if (len + 1 == bufsz && buf[len - 1] != '\n') {
        /* The line did not fit. Drain it so the next read is a real record. */
        int c;
        while ((c = fgetc(fp)) != EOF && c != '\n')
            ;
        if (too_long)
            *too_long = 1;
    }
    return 1;
}

/*
 * Parse one SYSUAF line into a record. 'line' is modified in place.
 *
 * FIELDS ARE SPLIT ON EVERY SEPARATOR, INCLUDING CONSECUTIVE ONES. strtok()
 * treats a run of delimiters as one, which silently DROPS every empty field
 * and shifts every field after it one position early -- five of the six rows
 * OVMX ships have an empty field, so a strtok split misparsed nearly the
 * whole authorization database (vms-cb5 round 5, measured on the real runtime
 * by tests/qemu/test_syssvc_setuai.c).
 */
int sysuaf_parse_line(char *line, sysuaf_record_t *rec)
{
    if (!line || !rec)
        return -1;

    /* Comments and blank lines are not records and are not errors. */
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
        return 0;

    str_trim(line);
    if (line[0] == '\0' || line[0] == '#')
        return 0;

    char *fields[SYSUAF_FIELD_COUNT];
    int nf;
    {
        char *p = line;
        for (nf = 0; nf < SYSUAF_FIELD_COUNT && p; nf++) {
            fields[nf] = p;
            char *sep = strchr(p, SYSUAF_SEP_CHAR);
            if (sep) {
                *sep = '\0';
                p = sep + 1;
            } else {
                p = NULL;
            }
        }
    }

    if (nf < SYSUAF_MIN_FIELDS)
        return -1;

    memset(rec, 0, sizeof(*rec));
    strncpy(rec->username, fields[SYSUAF_F_USERNAME], sizeof(rec->username) - 1);
    str_upcase(rec->username);
    strncpy(rec->password_hash, fields[SYSUAF_F_PWHASH],
            sizeof(rec->password_hash) - 1);
    /*
     * ============================================================
     * SYSUAF.DAT's UIC FIELDS ARE OCTAL (vms-e60)
     * ============================================================
     * These were strtoul(..., 10). That is the bug, and which base is right
     * was DERIVED from the oracle rather than picked:
     *
     *   ORACLE (measured twice on lab nodes vax3/vax2, and asserted in-tree
     *   at tests/qemu/test_syssvc_ident.c):
     *       F$IDENTIFIER("DEFAULT","NAME_TO_NUMBER") -> 8388736
     *       = %X00800080 -> group 0x80 = 128, member 0x80 = 128, which is
     *       UIC [200,200] written the way VMS writes UICs.
     *
     *   SYSUAF.DAT ships 'DEFAULT||200|200|...'. Read as OCTAL that is
     *   128/128 and reproduces the oracle exactly. Read as DECIMAL it is
     *   200/200 -> 13107400, which matches nothing.
     *
     * SYSTEM's 1|4 reads identically in both bases -- that is the coincidence
     * that hid this for as long as SYSTEM was the only account anyone logged
     * in as.
     *
     * VMS's own convention is quoted in this tree at
     * src/libvms/include/ovmx_secparam.h and docs/oracle/vax73-privileges.md:
     * "bear in mind that numbers in a UIC are octal".
     *
     * THE BASE IS NOW A SHARED CONSTANT (SYSUAF_UIC_RADIX), and this is the
     * only site that applies it to a file field. When there were five parsers
     * a partial base change did not restore the old behaviour, it RELOCATED
     * the disagreement to a new pair of subsystems -- measured by the UAT:
     * fixing ten sites and leaving PID 1's decimal made every non-SYSTEM
     * login unable to write its own login directory (GUEST's home owned by
     * 201:200 while GUEST ran as 129:128, so COPY returned %RMS-E-CRE).
     * ============================================================
     */
    rec->uic_group  = (uint32_t)strtoul(fields[SYSUAF_F_UIC_GROUP], NULL,
                                        SYSUAF_UIC_RADIX);
    rec->uic_member = (uint32_t)strtoul(fields[SYSUAF_F_UIC_MEMBER], NULL,
                                        SYSUAF_UIC_RADIX);
    strncpy(rec->default_dir, fields[SYSUAF_F_DEFDIR],
            sizeof(rec->default_dir) - 1);
    if (nf > SYSUAF_F_FLAGS)
        strncpy(rec->flags, fields[SYSUAF_F_FLAGS], sizeof(rec->flags) - 1);
    if (nf > SYSUAF_F_PRIVILEGES)
        strncpy(rec->privileges, fields[SYSUAF_F_PRIVILEGES],
                sizeof(rec->privileges) - 1);
    /* LGICMD is the trailing OPTIONAL field (vms-e48). A legacy 5-7 field row
     * leaves it empty, which sysuaf_login_command_file() reads as "use the
     * documented SYS$LOGIN:LOGIN.COM default". */
    if (nf > SYSUAF_F_LGICMD)
        strncpy(rec->lgicmd, fields[SYSUAF_F_LGICMD],
                sizeof(rec->lgicmd) - 1);

    return 1;
}

/*
 * Render one record as the line that represents it (no trailing newline).
 *
 * ONE FORMAT STRING. There used to be two, and they disagreed on the FLAGS
 * field: tools/vms_authorize.c wrote it "%s" and src/libvms/syssvc/sys_uai.c
 * wrote it "%u", so AUTHORIZE round-tripped an empty FLAGS as empty while
 * $SETUAI rewrote it as "0" -- the same field of the same file with two
 * answers. FLAGS is a NAME LIST in the file (see sysuaf_flags_to_mask); the
 * longword form belongs to the $GETUAI/$SETUAI item code, not to the file.
 *
 * %o on the two UIC fields, from the same SYSUAF_UIC_RADIX the parse uses --
 * writing them decimal would make the writer unable to read back what it
 * just saved.
 *
 * OVER-LENGTH IS REFUSED, NOT CLIPPED. Returns -1 and leaves 'out' untouched
 * when the record does not fit within SYSUAF_LINE_MAX including the newline a
 * caller will append. This is the loud writer-side failure the silent reader
 * truncation made necessary: the caller must report it and must not write a
 * record it cannot read back.
 */
int sysuaf_format_record(const sysuaf_record_t *rec, char *out, size_t outsz)
{
    if (!rec || !out || outsz == 0)
        return -1;

    /*
     * LGICMD (vms-e48) is written as a trailing field ONLY when it is set.
     * An account with no LGICMD formats to the exact 7-field line it did
     * before this field existed, so every pre-vms-e48 SYSUAF.DAT row (all of
     * which have an empty LGICMD) round-trips byte-identically -- the writer
     * gains a field without rewriting the rest of the file. This is still ONE
     * format: the optional tail mirrors how FLAGS/PRIVILEGES are already
     * optional on the read side.
     */
    char tmp[SYSUAF_LINE_MAX * 2];
    int n;
    if (rec->lgicmd[0]) {
        n = snprintf(tmp, sizeof(tmp), "%s|%s|%o|%o|%s|%s|%s|%s",
                     rec->username,
                     rec->password_hash,
                     (unsigned)rec->uic_group,
                     (unsigned)rec->uic_member,
                     rec->default_dir,
                     rec->flags,
                     rec->privileges,
                     rec->lgicmd);
    } else {
        n = snprintf(tmp, sizeof(tmp), "%s|%s|%o|%o|%s|%s|%s",
                     rec->username,
                     rec->password_hash,
                     (unsigned)rec->uic_group,
                     (unsigned)rec->uic_member,
                     rec->default_dir,
                     rec->flags,
                     rec->privileges);
    }

    /* n + 1 for the newline the caller appends; the line must fit in
     * SYSUAF_LINE_MAX so that sysuaf_read_line() can read it back whole. */
    if (n < 0 || (size_t)n + 1 >= SYSUAF_LINE_MAX)
        return -1;
    if ((size_t)n >= outsz)
        return -1;

    memcpy(out, tmp, (size_t)n + 1);
    return n;
}

/* ------------------------------------------------------------------ */
/* FLAGS: names in the file, a longword in the API                     */
/* ------------------------------------------------------------------ */

static const struct { const char *name; uint32_t bit; } sysuaf_flag_names[] = {
    { "DISCTLY",      UAI$M_DISCTLY },
    { "DEFCLI",       UAI$M_DEFCLI },
    { "LOCKPWD",      UAI$M_LOCKPWD },
    { "DISMAIL",      UAI$M_DISMAIL },
    { "CAPTIVE",      UAI$M_CAPTIVE },
    { "DISREPORT",    UAI$M_DISREPORT },
    { "DISRECONNECT", UAI$M_DISRECONNECT },
    { "AUTOLOGIN",    UAI$M_AUTOLOGIN },
    { "DISLOCAL",     UAI$M_DISLOCAL },
    { "DISDIALUP",    UAI$M_DISDIALUP },
    { "DISNETWORK",   UAI$M_DISNETWORK },
    { "DISACNT",      UAI$M_DISACNT },
    { "DISBATCH",     UAI$M_DISBATCH },
    { "DISUSER",      UAI$M_DISUSER },
    { "DISWELCOME",   UAI$M_DISWELCOME },
    { "EXTAUTH",      UAI$M_EXTAUTH },
    { "PWDMIX",       UAI$M_PWDMIX },
    { "GENERATE_PWD", UAI$M_GENERATE_PWD },
};

uint32_t sysuaf_flags_to_mask(const char *flags)
{
    if (!flags || flags[0] == '\0')
        return 0;

    char buf[64];
    strncpy(buf, flags, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint32_t mask = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, ",", &saveptr); tok;
         tok = strtok_r(NULL, ",", &saveptr)) {
        while (*tok == ' ') tok++;
        for (size_t i = 0; i < sizeof(sysuaf_flag_names) /
                               sizeof(sysuaf_flag_names[0]); i++) {
            if (strcasecmp(tok, sysuaf_flag_names[i].name) == 0) {
                mask |= sysuaf_flag_names[i].bit;
                break;
            }
        }
    }
    return mask;
}

void sysuaf_mask_to_flags(uint32_t mask, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    out[0] = '\0';

    /* Mask 0 renders as the EMPTY string, so that a row whose FLAGS field is
     * empty round-trips through $GETUAI/$SETUAI back to empty. Rendering it
     * as "0" is what the second writer did, and is why the same field of the
     * same file had two representations. */
    size_t used = 0;
    for (size_t i = 0; i < sizeof(sysuaf_flag_names) /
                           sizeof(sysuaf_flag_names[0]); i++) {
        if (!(mask & sysuaf_flag_names[i].bit))
            continue;
        size_t nlen = strlen(sysuaf_flag_names[i].name);
        size_t need = nlen + (used ? 1 : 0);
        if (used + need + 1 > outsz)
            break;
        if (used)
            out[used++] = ',';
        memcpy(out + used, sysuaf_flag_names[i].name, nlen);
        used += nlen;
        out[used] = '\0';
    }
}

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
    char sysuaf_linux[SYSUAF_LINE_MAX];
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

    char line[SYSUAF_LINE_MAX];
    int too_long = 0;
    while (sysuaf_read_line(fp, line, sizeof(line), &too_long)) {
        /*
         * A LINE THAT DID NOT FIT IS NOT A RECORD. It is reported and
         * skipped, never parsed as the short row its prefix resembles --
         * that silent downgrade is what made SYSTEM's row look like it had
         * no privileges field and halted the boot (vms-9b7).
         */
        if (too_long) {
            fprintf(stderr, "%%SYSUAF-E-RECTOOLONG, record longer than %d "
                            "bytes in %s -- ignored\n",
                    SYSUAF_LINE_MAX - 1, SYSUAF_PATH);
            continue;
        }

        /* Every row is parsed before either direction decides whether it
         * matched, so the UIC direction reads the SAME parse the name
         * direction does rather than a second copy of it. */
        sysuaf_record_t row;
        if (sysuaf_parse_line(line, &row) != 1)
            continue;

        if (username
            ? strcmp(row.username, search_copy) == 0
            : ((row.uic_group << 16) | row.uic_member) == want_uic) {
            *out = row;
            fclose(fp);
            return 0;
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
/* sysuaf_write_record                                                 */
/* ------------------------------------------------------------------ */

/*
 * Rewrite ONE existing row of SYSUAF_PATH in place. See the doc comment on
 * this declaration in sysuaf.h for the contract; this is the SAME targeted-
 * rewrite shape as sys$setuai's (src/libvms/syssvc/sys_uai.c) -- read every
 * row, replace the one whose username matches, copy every other row
 * verbatim, then atomically rename the result over the original. Added so a
 * second caller (DCL's SET PASSWORD, vms-e9e) does not have to hand-roll a
 * THIRD copy of this loop to reach the one shared format
 * (sysuaf_format_record()) -- it now reaches the loop too.
 */
int sysuaf_write_record(const sysuaf_record_t *rec)
{
    if (!rec || rec->username[0] == '\0')
        return -1;

    char sysuaf_linux[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux, sizeof(sysuaf_linux));

    char tmp_path[1040];
    snprintf(tmp_path, sizeof(tmp_path), "%s.TMP", sysuaf_linux);

    FILE *in = fopen(sysuaf_linux, "r");
    if (!in)
        return -1;

    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        return -1;
    }

    int found = 0;
    int failed = 0;
    char line[SYSUAF_LINE_MAX];
    int too_long = 0;

    while (sysuaf_read_line(in, line, sizeof(line), &too_long)) {
        if (too_long) {
            /* Cannot copy a prefix (corrupts the account) or drop the row
             * (deletes it). The only answer that loses nothing is to
             * abandon the whole rewrite and leave the original untouched. */
            failed = 1;
            break;
        }

        /* sysuaf_parse_line() modifies its argument, so the verbatim copy
         * has to be taken BEFORE the parse. */
        char verbatim[SYSUAF_LINE_MAX];
        strncpy(verbatim, line, sizeof(verbatim) - 1);
        verbatim[sizeof(verbatim) - 1] = '\0';

        sysuaf_record_t row;
        int rc = sysuaf_parse_line(line, &row);
        if (rc == 1 && strcasecmp(row.username, rec->username) == 0) {
            char out_line[SYSUAF_LINE_MAX];
            if (sysuaf_format_record(rec, out_line, sizeof(out_line)) < 0) {
                failed = 1;
                break;
            }
            fprintf(out, "%s\n", out_line);
            found = 1;
        } else {
            fputs(verbatim, out);
        }
    }

    fclose(in);
    fclose(out);

    if (failed || !found) {
        unlink(tmp_path);
        return -1;
    }

    if (rename(tmp_path, sysuaf_linux) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
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

/* ------------------------------------------------------------------ */
/* sysuaf_login_command_file (vms-e48)                                 */
/* ------------------------------------------------------------------ */

void sysuaf_login_command_file(const sysuaf_record_t *rec,
                               char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    const char *spec = (rec && rec->lgicmd[0]) ? rec->lgicmd
                                               : SYSUAF_DEFAULT_LGICMD;
    snprintf(out, outsz, "%s", spec);
}
