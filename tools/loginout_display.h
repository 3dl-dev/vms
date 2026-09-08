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

/*
 * loginout_display_system_identification - the LOGINOUT SYSTEM-IDENTIFICATION
 * line, printed ONCE immediately before the "Username:" prompt (vms-3e9,
 * operator-reported on a real VAX boot: OVMX had no pre-Username announcement
 * at all, on ANY arch).
 *
 * ORACLE (docs/design-boot-faithful.md §2 and §3.5, the OpenVMS Alpha V8.4
 * capture; the same shape is in docs/oracle/installation-media-vax73-alpha84.md
 * §5): the last thing a VMS console prints before the login prompt is an
 * identification line, ONE leading space, then a blank line, then "Username:":
 *
 *      Welcome to OpenVMS (TM) Alpha Operating System, Version V8.4
 *
 *     Username:
 *
 * It is a DIFFERENT emission from the post-authentication SYS$WELCOME (which
 * the same capture shows indented THREE spaces, after the password is
 * accepted) and from the boot identification banner
 * (src/ovmx_init/ovmx_init.c, printed by PID 1 long before LOGINOUT exists).
 * All three now exist in OVMX, once each, in that order.
 *
 * WHY THE WORDING IS OVMX'S OWN, NOT THE ORACLE'S BYTES (INV-0, the trademark
 * ceiling). OVMX may not print "Welcome to OpenVMS ... Version V8.4" on a
 * human surface: that claims to BE VSI's product. The oracle's SHAPE is
 * reproduced -- welcome, product, architecture, "Operating System, Version",
 * version, one leading space, blank line, prompt -- with OVMX's own badged
 * identity in it.
 *
 * DELIBERATELY NOT MATCHED BY 'Welcome to <product>' (anti-hollowing). The
 * post-authentication SYS$WELCOME default reads "Welcome to OpenVMX V0.x ...",
 * and roughly thirty runtime gates use that exact substring as their
 * PROOF THAT A LOGIN SUCCEEDED. A pre-Username line that also matched it would
 * silently turn every one of them into a test that passes without a login. So
 * this line reads "Welcome to the <product> ..." -- one word different, and the
 * difference is load-bearing: tests/tools/test_loginout_display.c asserts the
 * two strings cannot be confused, so a future reword that reintroduces the
 * collision reds there instead of quietly hollowing the battery.
 *
 * @out      destination stream (nothing is emitted if NULL).
 * @product  human-facing product name -- OVMX_PRODUCT_NAME (INV-1 SSOT).
 * @arch     the architecture this build actually runs on -- ovmx_hw_arch().
 *           NULL/empty omits the architecture word rather than guessing one.
 * @version  human-facing version -- ovmx_product_version() (INV-1 SSOT).
 * @badge    the INV-0 compatibility badge (OVMX_COMPAT_BADGE), or NULL to omit.
 *
 * Every value is passed in from the identity SSOT; this TU holds no identity
 * string of its own (same rule as the session-info block above). If product or
 * version is missing NOTHING is printed -- a half-known identity is not
 * completed with an invented half (INV-6).
 */
void loginout_display_system_identification(FILE *out,
                                            const char *product,
                                            const char *arch,
                                            const char *version,
                                            const char *badge);

#endif /* LOGINOUT_DISPLAY_H */
