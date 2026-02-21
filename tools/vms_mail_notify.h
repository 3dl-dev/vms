/*
 * vms_mail_notify.h - VMS MAIL login notification support
 *
 * Provides a declaration for mail_count_unread() so that vms_login.c
 * (or any other login-time code) can check for new mail and display
 * a notification without pulling in the full mail implementation.
 *
 * Integration:
 *   After a successful login, call mail_count_unread(username) and
 *   if the result > 0 display:
 *     printf("You have %d new mail message%s.\n", n, n != 1 ? "s" : "");
 *
 * The actual implementation lives in vms_mail.c (compiled into vms_mail
 * binary).  This header is for build-time reference; link against
 * vms_mail.o if you need the function at link time.  The recommended
 * approach for vms_login is to exec a small check after login rather
 * than linking directly, since vms_mail is a standalone utility.
 */

#ifndef VMS_MAIL_NOTIFY_H
#define VMS_MAIL_NOTIFY_H

/*
 * mail_count_unread - Count unread messages for a VMS user.
 *
 * @username  VMS username (will be uppercased internally).
 * @returns   Number of unread, non-deleted messages; 0 on any error.
 */
int mail_count_unread(const char *username);

#endif /* VMS_MAIL_NOTIFY_H */
