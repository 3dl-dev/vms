/*
 * ovmx_accounting.c - OVMX Login/Logout Accounting
 *
 * Per-user last-login timestamp tracking + the system-wide accounting-enabled
 * flag, now reached the VMS way: RMS $OPEN/$GET and $CREATE/$PUT over the
 * Files-11 ODS-2 ACP (vms-274, epic vms-208), NOT fopen/mkdir on the /vms
 * passthrough (which is being retired -- docs/design-files11-acp-executive.md).
 *
 * STORAGE LAYOUT (OVMX design choice). Last-login is an OVMX-ism -- real
 * OpenVMS keeps LOGIN/last-login fields inside SYSUAF, not in a side file. OVMX
 * had stored one file per user under a SYS$MANAGER:LASTLOGIN *subdirectory*,
 * which meant a per-boot mkdir. Reaching that the VMS way would be an
 * IO$_CREATE of a LASTLOGIN.DIR; instead OVMX stores each user's record as a
 * FLAT file "SYS$MANAGER:LASTLOGIN_<USER>.DAT" in the already-existing
 * SYS$MANAGER directory. This removes the /vms mkdir entirely (there is no
 * subdirectory to create) while keeping the write a genuine RMS $CREATE/$PUT
 * onto the ODS-2 SYS$DISK. Labelled here as an OVMX choice, not VMS-authentic.
 *
 * FAIL-HONEST (Rule 9 / INV-6): with no mounted ACP volume / no /dev/vms these
 * reads and writes fail (no lines, -1); nothing falls back to a private POSIX
 * copy. The accounting-enabled default remains VMS's ACC$START-at-boot value
 * (enabled) when the flag file is simply absent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "ovmx_accounting.h"
#include "rms_textfile.h"

/* ------------------------------------------------------------------ */
/* Build the VMS filespec for a user's last-login record.             */
/* username is upper-cased automatically.                             */
/* ------------------------------------------------------------------ */
static void lastlogin_spec(const char *username, char *buf, size_t bufsiz)
{
    char upper[64];
    size_t i;
    for (i = 0; i < sizeof(upper) - 1 && username[i]; i++)
        upper[i] = (char)toupper((unsigned char)username[i]);
    upper[i] = '\0';

    /* Flat per-user record in SYS$MANAGER (see the header comment). */
    snprintf(buf, bufsiz, "SYS$MANAGER:LASTLOGIN_%s.DAT", upper);
}

/* ------------------------------------------------------------------ */
/* Query the real, system-wide accounting-enabled flag.               */
/* No file yet == enabled (matches OpenVMS ACC$START at boot).        */
/* ------------------------------------------------------------------ */
int ovmx_accounting_is_enabled(void)
{
    rms_textfile_t *fp = rms_textfile_open(VMS_ACCOUNTING_STATE_PATH);
    if (!fp)
        return 1;   /* absent/unreachable flag => VMS default (enabled) */

    char line[16];
    int have = rms_textfile_getline(fp, line, sizeof(line), NULL);
    rms_textfile_close(fp);

    /* Any content other than a leading '0' means enabled; this also covers a
     * truncated/corrupt record the same way a fresh boot's missing file does
     * -- fail toward the VMS default (enabled), never toward a silent,
     * unobservable disable. */
    if (!have)
        return 1;
    return (line[0] != '0');
}

/* ------------------------------------------------------------------ */
/* Flip the real, system-wide accounting-enabled flag. Persisted, so   */
/* every process -- not just the caller -- observes it.               */
/* ------------------------------------------------------------------ */
int ovmx_accounting_set_enabled(int enabled)
{
    return rms_textfile_write_line(VMS_ACCOUNTING_STATE_PATH,
                                   enabled ? "1" : "0");
}

/* ------------------------------------------------------------------ */
/* Read the last-login timestamp for a user.                          */
/* Returns 0 on success, -1 if no record or parse error.             */
/* ------------------------------------------------------------------ */
int ovmx_accounting_get_lastlogin(const char *username, time_t *t)
{
    if (!username || !t) return -1;

    char spec[128];
    lastlogin_spec(username, spec, sizeof(spec));

    rms_textfile_t *fp = rms_textfile_open(spec);
    if (!fp) return -1;

    char line[32];
    int have = rms_textfile_getline(fp, line, sizeof(line), NULL);
    rms_textfile_close(fp);
    if (!have) return -1;

    long long ts = 0;
    if (sscanf(line, "%lld", &ts) != 1 || ts <= 0) return -1;

    *t = (time_t)ts;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Record a new login timestamp for a user.                           */
/* Writes current time to SYS$MANAGER:LASTLOGIN_<USER>.DAT via RMS.    */
/* Returns 0 on success, -1 on failure.                               */
/* ------------------------------------------------------------------ */
int ovmx_accounting_record_login(const char *username)
{
    if (!username) return -1;

    /* vms-17d (INV-DCL): SET ACCOUNTING/DISABLE must actually stop recording,
     * not just flip a bool nothing reads. */
    if (!ovmx_accounting_is_enabled())
        return 0;

    char spec[128];
    lastlogin_spec(username, spec, sizeof(spec));

    char ts[32];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));

    /* Genuine RMS $CREATE + $PUT onto the ODS-2 SYS$DISK -- fail-honest with no
     * ACP volume, never a silent POSIX write (Rule 9 / INV-6). */
    return rms_textfile_write_line(spec, ts);
}
