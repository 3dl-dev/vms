/*
 * mail_notify.c - Shared VMS MAIL storage-location + new-mail-count helpers.
 *
 * ONE IMPLEMENTATION (CLAUDE.md Rule 2, no facades / no divergent copies):
 * get_user_homedir(), build_maildir() and mail_count_unread() were duplicated
 * ONLY inside vms_mail.c, so the login-time new-mail notification promised by
 * vms_mail_notify.h had no way to call the real counter without either linking
 * MAIL's whole main() or re-implementing the maildir layout a second time.
 * They live here now and are compiled into BOTH vms_mail and vms_login, so the
 * mailbox MAIL writes to is byte-identically the one the login notification
 * reads. The bodies moved verbatim from vms_mail.c -- no behaviour change.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vms_mail_notify.h"
#include "ovmx_layout.h"
#include "str_util.h"
/* SYSUAF is the sole VMS account database — binary indexed lookup (vms-d92). */
#include "sysuaf.h"

/* Get home directory for a VMS username (its SYSUAF default-directory field).
 *
 * The lookup is a binary indexed read (vms-d92) -- no ASCII pipe-parse.
 *
 * DELETED, NOT REPLACED (vms-a30): the /etc/passwd fallback that once stood
 * here handed back a Linux home directory as a VMS account's default
 * directory. SYSUAF answers this or nothing does. */
int get_user_homedir(const char *username, char *homedir, size_t sz)
{
    sysuaf_record_t rec;
    if (sysuaf_lookup(username, &rec) == 0) {
        strncpy(homedir, rec.default_dir, sz - 1);
        homedir[sz - 1] = '\0';
        return 0;
    }
    return -1;
}

/* Build maildir path for a given username. */
void build_maildir(const char *username, char *out, size_t sz)
{
    char homedir[4096];
    if (get_user_homedir(username, homedir, sizeof(homedir)) != 0) {
        /* Fallback */
        snprintf(homedir, sizeof(homedir), "/home/%s", username);
    }
    snprintf(out, sz, "%s/%s", homedir, MAIL_SUBDIR);
}

/* Count unread, non-deleted messages for a user (0 on any error). */
int mail_count_unread(const char *username)
{
    char maildir[4096];
    build_maildir(username, maildir, sizeof(maildir));

    char idxpath[4096];
    snprintf(idxpath, sizeof(idxpath), "%s/%s", maildir, MAIL_INDEX);

    FILE *fp = fopen(idxpath, "r");
    if (!fp) return 0;

    int count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        str_trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        /* Parse: NUMBER|READ|DELETED|... */
        char tmp[4096];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *tok = strtok(tmp, "|");
        if (!tok) continue; /* number */
        tok = strtok(NULL, "|");
        if (!tok) continue;
        int read = atoi(tok);
        tok = strtok(NULL, "|");
        if (!tok) continue;
        int deleted = atoi(tok);

        if (!read && !deleted) count++;
    }
    fclose(fp);
    return count;
}
