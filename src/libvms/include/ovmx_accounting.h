/*
 * ovmx_accounting.h - OVMX Login/Logout Accounting
 *
 * Per-user last-login timestamps are stored in individual files under
 * SYS$MANAGER:LASTLOGIN/<USERNAME> (uppercase, one file per user).
 *
 * File format (text, one line):
 *   <unix-timestamp>\n
 *
 * This is intentionally simple — just enough to satisfy "Last interactive
 * login" on OpenVMS, which shows the login time from the previous session.
 *
 * vms-17d (INV-DCL, docs/design-dcl-fidelity.md sec 5): this module also
 * owns the REAL, system-wide, persisted "is accounting enabled" flag that
 * DCL's SET ACCOUNTING/SHOW ACCOUNTING read and write
 * (VMS_ACCOUNTING_STATE_PATH, ovmx_layout.h). Before this, SET ACCOUNTING
 * toggled a per-DCL-context bool nothing else -- including this module's
 * own ovmx_accounting_record_login(), called from login/SSH -- could see,
 * so the command controlled nothing. Every record path now checks
 * ovmx_accounting_is_enabled() first.
 */

#ifndef __OVMX_ACCOUNTING_H
#define __OVMX_ACCOUNTING_H

#include <stdint.h>
#include <time.h>
#include "ovmx_layout.h"

/* Directory that holds per-user last-login timestamp files */
#define OVMX_LASTLOGIN_DIR  VMS_LASTLOGIN_DIR

/*
 * ovmx_accounting_get_lastlogin - Read the previous login time for a user.
 *
 * @param username  Username (case-insensitive; stored as uppercase)
 * @param t         Receives the Unix timestamp of last login
 *
 * Returns 0 on success (record found), -1 if no record exists.
 */
int ovmx_accounting_get_lastlogin(const char *username, time_t *t);

/*
 * ovmx_accounting_record_login - Record a new login for a user.
 *
 * Writes the current time to SYS$MANAGER:[LASTLOGIN]<USERNAME>.
 * Creates the directory if needed.
 *
 * @param username  Username (case-insensitive; stored as uppercase)
 *
 * Returns 0 on success, -1 on failure.
 */
int ovmx_accounting_record_login(const char *username);

/*
 * ovmx_accounting_is_enabled - Query the real, system-wide accounting flag.
 *
 * Reads VMS_ACCOUNTING_STATE_PATH (SYS$MANAGER:ACCOUNTNG.ENB). If the file
 * does not exist yet, accounting is ENABLED by default -- matching real
 * OpenVMS, where accounting is running (started by ACC$START) from system
 * startup unless a manager explicitly disables it.
 *
 * Returns 1 if accounting is enabled, 0 if disabled.
 */
int ovmx_accounting_is_enabled(void);

/*
 * ovmx_accounting_set_enabled - Flip the real, system-wide accounting flag.
 *
 * Persists to VMS_ACCOUNTING_STATE_PATH so the state is visible to every
 * process (SHOW ACCOUNTING, a later login recording a event, a later DCL
 * session), not just the caller's own process -- this is what SET
 * ACCOUNTING /ENABLE and /DISABLE call.
 *
 * @param enabled  Nonzero to enable, 0 to disable.
 *
 * Returns 0 on success, -1 on failure.
 */
int ovmx_accounting_set_enabled(int enabled);

#endif /* __OVMX_ACCOUNTING_H */
