/*
 * loginout_display.h - OpenVMS-faithful LOGINOUT session-information block.
 *
 * The lines LOGINOUT prints AFTER successful authentication and AFTER the
 * SYS$WELCOME banner, in the order and format OpenVMS uses:
 *
 *     Last interactive login on <dd-MMM-YYYY hh:mm:ss.cc>
 *     Last non-interactive login on <dd-MMM-YYYY hh:mm:ss.cc>
 *     <N> failures since last successful login
 *     You have <N> new mail message(s).
 *
 * ORACLE / CLEAN-ROOM (CLAUDE.md Rule 8). Wording, ordering and the
 * omit-when-absent behaviour are taken from public OpenVMS documentation,
 * NOT from disassembly:
 *   - VSI OpenVMS Guide to System Security, "Reading Informational Messages"
 *     (mirror: mrynet.com/FTP/os/VMS/docs/ssb71/6346/6346p002.htm) gives the
 *     verbatim lines "Last interactive login on ...",
 *     "Last non-interactive login on ..." and "N failures since last
 *     successful login" (no trailing period), and states each is reset every
 *     login and OMITTED when its value is absent -- e.g. an interactive login
 *     with no bad-password attempts shows neither the non-interactive nor the
 *     failures line.
 *   - The same source and the OpenVMS MAIL utility give "You have 1 new mail
 *     message." (singular) / "... new mail messages." (plural), shown only
 *     when unread mail exists.
 *   - The timestamp is the OpenVMS standard absolute date-time format
 *     dd-MMM-YYYY hh:mm:ss.cc ($ASCTIME / F$TIME), space-padded day.
 *
 * NO FACADES (CLAUDE.md Rule 9 / INV-6). Each line is emitted ONLY when its
 * value is real and present. A zero/absent value prints nothing -- exactly as
 * OpenVMS does -- rather than inventing a placeholder date, a "0 failures"
 * line, or a "no previous login" sentence VMS never prints.
 *
 * IDENTITY (INV-0). This block prints NO product identity string; the badged
 * OpenVMX/"OpenVMS-compatible" welcome is SYS$WELCOME's job
 * (ovmx_banner_welcome, ovmx_banner.h). Keeping identity out of here means the
 * one honest-identity surface stays in one place.
 */

#ifndef LOGINOUT_DISPLAY_H
#define LOGINOUT_DISPLAY_H

#include <stdio.h>
#include <time.h>

/*
 * loginout_display_session_info - Emit the post-authentication login-info
 * block to 'out'.
 *
 * @out                  destination stream.
 * @last_interactive     time of the previous interactive login, or 0/absent
 *                       to omit the "Last interactive login" line.
 * @last_noninteractive  time of the previous non-interactive (batch/network)
 *                       login, or 0/absent to omit the line.
 * @login_failures       failed login attempts since the last success; 0 omits
 *                       the failures line. Pluralised ("1 failure",
 *                       "N failures"); no trailing period.
 * @new_mail_count       unread mail messages; <= 0 omits the mail line.
 *                       Pluralised ("1 ... message.", "N ... messages.").
 *
 * Times are formatted in LOCAL time (VMS shows local time) as
 * dd-MMM-YYYY hh:mm:ss.cc. Each present line is indented four spaces.
 */
void loginout_display_session_info(FILE *out,
                                   time_t last_interactive,
                                   time_t last_noninteractive,
                                   unsigned login_failures,
                                   int new_mail_count);

#endif /* LOGINOUT_DISPLAY_H */
