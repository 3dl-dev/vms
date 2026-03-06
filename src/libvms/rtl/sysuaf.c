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

        /* Case-insensitive compare */
        char uname_copy[64];
        strncpy(uname_copy, fields[0], sizeof(uname_copy) - 1);
        uname_copy[sizeof(uname_copy) - 1] = '\0';
        str_upcase(uname_copy);

        if (strcmp(uname_copy, search_copy) == 0) {
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
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;  /* User not found */
}

/* ------------------------------------------------------------------ */
/* sysuaf_authenticate                                                 */
/* ------------------------------------------------------------------ */

/*
 * Check that 'password' matches the stored hash in 'rec'.
 * An empty hash means no password is required — returns 1.
 * A non-empty hash is compared as a SHA256 hex string (case-insensitive).
 * Returns 1 on match, 0 on mismatch.
 */
int sysuaf_authenticate(const sysuaf_record_t *rec, const char *password)
{
    /* Empty hash = no password required */
    if (rec->password_hash[0] == '\0')
        return 1;

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
