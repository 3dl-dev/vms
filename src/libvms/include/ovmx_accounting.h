/*
 * ovmx_accounting.h - OVMX Login/Logout Accounting
 *
 * Per-user last-login timestamps are stored in individual files under
 * /etc/ovmx/lastlogin/<USERNAME> (uppercase, one file per user).
 *
 * File format (text, one line):
 *   <unix-timestamp>\n
 *
 * This is intentionally simple — just enough to satisfy "Last interactive
 * login" on OpenVMS, which shows the login time from the previous session.
 */

#ifndef __OVMX_ACCOUNTING_H
#define __OVMX_ACCOUNTING_H

#include <stdint.h>
#include <time.h>

/* Directory that holds per-user last-login timestamp files */
#define OVMX_LASTLOGIN_DIR  "/etc/ovmx/lastlogin"

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
 * Writes the current time to /etc/ovmx/lastlogin/<USERNAME>.
 * Creates the directory if needed.
 *
 * @param username  Username (case-insensitive; stored as uppercase)
 *
 * Returns 0 on success, -1 on failure.
 */
int ovmx_accounting_record_login(const char *username);

#endif /* __OVMX_ACCOUNTING_H */
