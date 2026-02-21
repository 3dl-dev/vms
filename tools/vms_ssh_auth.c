/*
 * vms_ssh_auth.c - PAM authentication helper for SSH
 *
 * Called by pam_exec to validate SSH passwords against sysuaf.dat.
 * Reads PAM_USER from environment, password from stdin (expose_authtok).
 * Exit 0 = success, non-zero = failure.
 *
 * Authentication logic is provided by the sysuaf library in libvms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "sysuaf.h"

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

    /* Look up user in SYSUAF */
    sysuaf_record_t rec;
    if (sysuaf_lookup(pam_user, &rec) < 0)
        return 1;  /* User not found */

    /* Authenticate */
    return sysuaf_authenticate(&rec, password) ? 0 : 1;
}
