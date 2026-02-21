/*
 * ovmx_accounting.c - OVMX Login/Logout Accounting
 *
 * Implements per-user last-login timestamp tracking.
 * Files live in /etc/ovmx/lastlogin/<USERNAME> (uppercase filenames).
 *
 * Thread-safe: file I/O is atomic enough for our single-login-per-user
 * use case. No locking needed for read; writes use a temp-rename pattern
 * for atomicity (best-effort on Linux).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "ovmx_accounting.h"

/* ------------------------------------------------------------------ */
/* Build the path to a user's last-login file.                        */
/* username is upper-cased automatically.                             */
/* ------------------------------------------------------------------ */
static void lastlogin_path(const char *username, char *buf, size_t bufsiz)
{
    char upper[64];
    size_t i;
    for (i = 0; i < sizeof(upper) - 1 && username[i]; i++)
        upper[i] = (char)toupper((unsigned char)username[i]);
    upper[i] = '\0';

    snprintf(buf, bufsiz, "%s/%s", OVMX_LASTLOGIN_DIR, upper);
}

/* ------------------------------------------------------------------ */
/* Ensure /etc/ovmx/lastlogin exists.                                 */
/* ------------------------------------------------------------------ */
static void ensure_dir(void)
{
    struct stat st;
    if (stat(OVMX_LASTLOGIN_DIR, &st) == 0)
        return;
    /* Try to create the hierarchy */
    mkdir("/etc/ovmx", 0755);
    mkdir(OVMX_LASTLOGIN_DIR, 0755);
}

/* ------------------------------------------------------------------ */
/* Read the last-login timestamp for a user.                          */
/* Returns 0 on success, -1 if no record or parse error.             */
/* ------------------------------------------------------------------ */
int ovmx_accounting_get_lastlogin(const char *username, time_t *t)
{
    if (!username || !t) return -1;

    char path[256];
    lastlogin_path(username, path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    long long ts = 0;
    int n = fscanf(fp, "%lld", &ts);
    fclose(fp);

    if (n != 1 || ts <= 0) return -1;

    *t = (time_t)ts;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Record a new login timestamp for a user.                           */
/* Writes current time to /etc/ovmx/lastlogin/<USERNAME>.            */
/* Returns 0 on success, -1 on failure.                               */
/* ------------------------------------------------------------------ */
int ovmx_accounting_record_login(const char *username)
{
    if (!username) return -1;

    ensure_dir();

    char path[256];
    lastlogin_path(username, path, sizeof(path));

    /* Write via temp file then rename for atomicity */
    char tmp[272];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        /* Fall back to direct write if tmp fails */
        fp = fopen(path, "w");
        if (!fp) return -1;
        fprintf(fp, "%lld\n", (long long)time(NULL));
        fclose(fp);
        return 0;
    }

    fprintf(fp, "%lld\n", (long long)time(NULL));
    fclose(fp);

    if (rename(tmp, path) != 0) {
        /* rename failed — try direct write */
        unlink(tmp);
        fp = fopen(path, "w");
        if (!fp) return -1;
        fprintf(fp, "%lld\n", (long long)time(NULL));
        fclose(fp);
    }

    return 0;
}
