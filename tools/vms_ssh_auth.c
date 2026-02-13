/*
 * vms_ssh_auth.c - PAM authentication helper for SSH
 *
 * Called by pam_exec to validate SSH passwords against sysuaf.dat.
 * Reads PAM_USER from environment, password from stdin (expose_authtok).
 * Exit 0 = success, non-zero = failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "sha256.h"

#define SYSUAF_PATH  "/etc/ovmx/sysuaf.dat"

static void upcase(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

static void trim_trailing(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t'))
        s[--len] = '\0';
}

int main(void)
{
    const char *pam_user = getenv("PAM_USER");
    if (!pam_user || pam_user[0] == '\0')
        return 1;

    /* Read password from stdin (pam_exec expose_authtok) */
    char password[128];
    if (fgets(password, sizeof(password), stdin) == NULL)
        password[0] = '\0';
    trim_trailing(password);

    /* Upcase the username for comparison */
    char username[64];
    strncpy(username, pam_user, sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';
    upcase(username);

    /* Search sysuaf.dat */
    FILE *fp = fopen(SYSUAF_PATH, "r");
    if (!fp)
        return 1;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        trim_trailing(line);

        /* Parse USERNAME:PASSWORD_HASH:... */
        char *fields[7];
        char *p = line;
        int nf = 0;

        for (nf = 0; nf < 7 && p; nf++) {
            fields[nf] = p;
            char *colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                p = colon + 1;
            } else {
                p = NULL;
            }
        }

        if (nf < 5)
            continue;

        char entry_name[64];
        strncpy(entry_name, fields[0], sizeof(entry_name) - 1);
        entry_name[sizeof(entry_name) - 1] = '\0';
        upcase(entry_name);

        if (strcmp(username, entry_name) != 0)
            continue;

        /* Found user — check password */
        const char *stored_hash = fields[1];

        if (stored_hash[0] == '\0') {
            /* Empty hash = no password required */
            fclose(fp);
            return 0;
        }

        char hex[65];
        sha256_hex((const uint8_t *)password, strlen(password), hex);

        if (strcasecmp(hex, stored_hash) == 0) {
            fclose(fp);
            return 0;
        }

        fclose(fp);
        return 1;
    }

    fclose(fp);
    return 1;  /* User not found */
}
