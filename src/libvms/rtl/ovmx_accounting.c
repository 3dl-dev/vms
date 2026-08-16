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
#include "vmsfs/filespec.h"
#include "ssdef.h"
#if defined(__linux__)
#include "vmsfs/sysdisk.h"   /* ODS-2 runtime flip (vms-d75): SYS$DISK write adapter */
#endif

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

    char lastlogin_dir_linux[1024];
    vmsfs_to_linux_path(OVMX_LASTLOGIN_DIR, lastlogin_dir_linux, sizeof(lastlogin_dir_linux));
    snprintf(buf, bufsiz, "%s/%s", lastlogin_dir_linux, upper);
}

/* ------------------------------------------------------------------ */
/* Ensure lastlogin directory exists on system disk.                   */
/* ------------------------------------------------------------------ */
static void ensure_dir(void)
{
    char lastlogin_dir_linux[1024];
    vmsfs_to_linux_path(OVMX_LASTLOGIN_DIR, lastlogin_dir_linux, sizeof(lastlogin_dir_linux));

#if defined(__linux__)
    /*
     * The accounting directory lives on SYS$DISK, so on Linux its creation is
     * rerouted onto the serialized genuine-ODS-2 write adapter (vms-d75, epic
     * vms-5eb, the ODS-2 runtime flip) -- never a POSIX mkdir on the /vms
     * passthrough. Best-effort like the POSIX mkdir it replaces: an
     * already-existing directory (or an absent parent) surfaces from the
     * adapter and record_login()'s subsequent create_file fails honestly if
     * the directory is genuinely missing. FAIL-HONEST (Rule 9 / INV-6): no
     * silent POSIX fallback for a SYS$DISK path.
     */
    if (ods2_sysdisk_owns_path(lastlogin_dir_linux)) {
        (void)ods2_sysdisk_mkdir(lastlogin_dir_linux);
        return;
    }
#endif

    struct stat st;
    if (stat(lastlogin_dir_linux, &st) == 0)
        return;
    mkdir(lastlogin_dir_linux, 0755);
}

/* ------------------------------------------------------------------ */
/* Build the path to the system-wide accounting-enabled flag file.    */
/* ------------------------------------------------------------------ */
static void accounting_state_path(char *buf, size_t bufsiz)
{
    vmsfs_to_linux_path(VMS_ACCOUNTING_STATE_PATH, buf, bufsiz);
}

/* ------------------------------------------------------------------ */
/* Ensure SYS$MANAGER exists on the system disk before writing into   */
/* it. On a real boot it always does (STARTUP.COM etc. already live   */
/* there); on host ctest -- no boot, no seeded system disk -- nothing */
/* has created it yet, so this recurses like the mkdir_p() every test */
/* fixture in this tree that bootstraps its own throwaway root uses.  */
/* ------------------------------------------------------------------ */
static void ensure_sysmgr_dir(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);

    /* Trim the trailing filename, leaving just the directory portion. */
    char *slash = strrchr(tmp, '/');
    if (!slash) return;
    *slash = '\0';

    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ------------------------------------------------------------------ */
/* Query the real, system-wide accounting-enabled flag.               */
/* No file yet == enabled (matches OpenVMS ACC$START at boot).        */
/* ------------------------------------------------------------------ */
int ovmx_accounting_is_enabled(void)
{
    char path[1024];
    accounting_state_path(path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (!fp) return 1;

    int c = fgetc(fp);
    fclose(fp);

    /* Any content other than a leading '0' means enabled; this also
     * covers a truncated/corrupt file the same way a fresh boot's
     * missing file does -- fail toward the VMS default (enabled), never
     * toward a silent, unobservable disable. */
    return (c != '0');
}

/* ------------------------------------------------------------------ */
/* Flip the real, system-wide accounting-enabled flag. Persisted, so   */
/* every process -- not just the caller -- observes it.               */
/* ------------------------------------------------------------------ */
int ovmx_accounting_set_enabled(int enabled)
{
    char path[1024];
    accounting_state_path(path, sizeof(path));

    ensure_sysmgr_dir(path);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%d\n", enabled ? 1 : 0);
    fclose(fp);
    return 0;
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

    /* vms-17d (INV-DCL): SET ACCOUNTING/DISABLE must actually stop
     * recording, not just flip a bool nothing reads. This is the check
     * that used to be missing -- record_login() ran unconditionally
     * regardless of what SET ACCOUNTING said. */
    if (!ovmx_accounting_is_enabled())
        return 0;

    ensure_dir();

    char path[256];
    lastlogin_path(username, path, sizeof(path));

#if defined(__linux__)
    /*
     * The LASTLOGIN record lives on SYS$DISK, so on Linux it is written
     * through the serialized genuine-ODS-2 write adapter (vms-d75, epic
     * vms-5eb, the ODS-2 runtime flip) -- never a plain fopen on the /vms
     * passthrough. The adapter is already single-writer-serialized by the
     * #614 flock broker, so no locking is added here.
     *
     * FAIL-HONEST (Rule 9 / INV-6): when SYS$DISK owns the path but no genuine
     * ODS-2 volume is registered, the write fails honestly (returns -1) --
     * NEVER a silent POSIX fallback (the LARP this rung exists to kill). The
     * temp+rename atomic-overwrite dance below is a POSIX-only idiom; on the
     * ODS-2 volume the record is created directly (ODS-2 is versioned, so a
     * genuine create is the faithful VMS write -- get_lastlogin reads back the
     * highest version). NOTE: rewriting an existing base version across
     * repeated logins is deferred to the flip's next-version handling; this
     * rung covers the boot/first-login write the atomic group needs.
     */
    if (ods2_sysdisk_owns_path(path)) {
        char record[64];
        int rlen = snprintf(record, sizeof(record), "%lld\n",
                            (long long)time(NULL));
        if (rlen < 0)
            return -1;
        size_t reclen = (rlen < (int)sizeof(record)) ? (size_t)rlen
                                                     : sizeof(record) - 1;
        int st = ods2_sysdisk_create_file(path, record, reclen);
        return $VMS_STATUS_SUCCESS(st) ? 0 : -1;
    }
#endif

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
