/*
 * dcl_in.h - DCL's one input source: where its command lines and its READ
 * SYS$INPUT / INQUIRE data come from (rd vms-4d27, vms-67a, vms-91fe,
 * vms-ee7). The output twin is dcl_out.h.
 *
 * SYS$INPUT names the process-permanent input stream. A terminal is read by
 * its driver ($QIO IO$_READPROMPT, dcl_terminal.c dcl_tt_read*), which every
 * caller tries first; a mailbox is read here by $QIOW IO$_READVBLK, a message
 * a record; a disk file by RMS $GET. dcl_in_get is the one call for what is
 * not the terminal driver.
 */
#ifndef OVMX_DCL_IN_H
#define OVMX_DCL_IN_H

#include <stddef.h>

#define DCL_IN_EOF   (-1)   /* end of the stream (or a read that failed) */

#define DCL_IN_ROUTE_NONE  0   /* the substrate stream (the stdio fallback) */
#define DCL_IN_ROUTE_MBX   1   /* a mailbox, by $QIOW IO$_READVBLK */
#define DCL_IN_ROUTE_FILE  2   /* a disk file, by RMS $GET */

/* Bind SYS$INPUT: a mailbox it names is $ASSIGNed, a disk file it names is
 * RMS $OPENed (a process whose substrate fd 0 is a terminal keeps the terminal
 * driver). Returns the DCL_IN_ROUTE_* now in effect. */
int dcl_in_bind_ppf(void);

/* The route bound (DCL_IN_ROUTE_*). */
int dcl_in_route(void);

/* Does the equivalence string `equiv` name the stream SYS$INPUT is bound to?
 * (READ SYS$INPUT then reads the next record of THAT stream.) */
int dcl_in_is_ppf(const char *equiv);

/* The next record of SYS$INPUT, without its terminator, in buf (always
 * NUL-terminated). `prompt` (may be NULL) is written to SYS$OUTPUT first.
 * Returns the length, or DCL_IN_EOF. */
int dcl_in_get(const char *prompt, char *buf, size_t bufsz);

/* After DCL_IN_EOF on a substrate terminal stream (^Z with no executive
 * terminal driver): make the stream readable again. 1 when it was. */
int dcl_in_rearm(void);

/* Did the last read end the stream? (DCL_IN_EOF at the end, not an error) */
int dcl_in_at_eof(void);

/* Clear the end-of-stream state before a new run of reads (a terminal's
 * ^Z ends one read, not the stream). */
void dcl_in_clear_eof(void);

/* Hand `rec` back: the next dcl_in_get returns it (one record). DCL reads
 * an image's data records ahead to the next '$' record and keeps that one for
 * its own next command read. */
void dcl_in_unget(const char *rec);

/* While on, reads go to the substrate fd 0: a command procedure's in-stream
 * data block that dcl_sysinput_install put there (CREATE, an image). */
void dcl_in_substrate(int on);

/* Resolve `name` through the logical-name tables until it names a mailbox
 * device (MBAn:); 1 with the device in out. Shared with dcl_mbx.c. */
int dcl_in_resolve_mailbox(const char *name, char *out, size_t outsz);

/* Release the SYS$INPUT channel / RMS stream. */
void dcl_in_shutdown(void);

#endif /* OVMX_DCL_IN_H */
