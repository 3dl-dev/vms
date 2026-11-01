/*
 * vms_messages.h - DCL's message helpers.
 *
 * There is no DCL-private message catalog here. The table of MSG_* codes this
 * header used to carry was invented: made-up facility numbers (DCL=1, COPY=2,
 * ...), made-up message numbers, and texts that matched no VMS message. Two of
 * them leaked into $STATUS. Every condition DCL returns is now a real VMS one
 * (ssdef.h / rmsdef.h / climsgdef.h), and every condition's text comes from the
 * one catalog, src/libvms/status.c (vms$status_message).
 */

#ifndef DCL_VMS_MESSAGES_H
#define DCL_VMS_MESSAGES_H

#include <stdint.h>

/*
 * The STS$K severity letter: 0=W, 1=S, 2=E, 3=I, 4=F (stsdef.h). Any other
 * value is reported as E.
 */
char vms_severity_char(int severity);

/*
 * The text of the real VMS condition a substrate errno stands for, e.g.
 * ENOENT -> RMS$_FNF -> "file not found", read from the one message catalog.
 * An errno with no VMS counterpart gets a plain "unexpected error (errno N)":
 * no VMS facility, ident or code is claimed for it.
 */
const char *vms_strerror(int unix_errno);

/*
 * Translate a Linux path to VMS-format for error messages.
 * E.g., "/vms/home/system/test.txt" -> "SYS$SYSDEVICE:[HOME.SYSTEM]TEST.TXT"
 * Falls back to uppercase if translation fails.
 * Returns pointer to static buffer.
 */
const char *vms_error_path(const char *linux_path);

#endif /* DCL_VMS_MESSAGES_H */
