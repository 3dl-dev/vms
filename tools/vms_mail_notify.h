/*
 * vms_mail_notify.h - the OVMX MAIL store: where a user's mail lives, how it is
 * read, and the login-time new-mail count.
 *
 * ONE STORE, OVER RMS (rd vms-47fd). Every reader and writer of mail -- MAIL.EXE
 * (DIRECTORY/READ/SEND/DELETE), the DECnet MAIL-11 server MAIL_SERVER.EXE
 * (inbound NODE::USER delivery) and LOGINOUT's "You have N new mail messages"
 * -- reaches the SAME file, the user's mail file in the SYSUAF default
 * directory, through RMS ($OPEN/$CREATE/$CONNECT/$GET/$PUT) over the executive
 * ACP. It used to be a POSIX directory (~/.vmsmail, fopen) that no inbound
 * network delivery could share honestly; that path is gone, not kept as a
 * fallback (INV-6): a user whose mail file cannot be reached has no mail, and a
 * delivery that cannot be stored is reported failed.
 *
 * FORMAT (OVMX-original, LABELLED -- not the VMS MAIL.MAI ISAM layout, which is
 * why the file is not called MAIL.MAI): one stream-LF sequential file of
 * id-tagged records, appended only:
 *     H|<id>|<date>   F|<id>|<from>   T|<id>|<to>   C|<id>|<cc>   S|<id>|<subj>
 *     B|<id>|<body line>  ...   E|<id>          (a message; E commits it)
 *     R|<id>  (read)    D|<id>  (deleted)       (state changes)
 * <id> is 16 hex digits. Because every record names its message, appends by
 * MAIL.EXE and by the network server can interleave without corrupting either,
 * and a message whose E record never landed (a crash, a dropped link) is not a
 * message: it is never listed and was never acknowledged.
 */

#ifndef VMS_MAIL_NOTIFY_H
#define VMS_MAIL_NOTIFY_H

#include <stddef.h>
#include <stdint.h>

#define MAIL_STORE_FILE   "OVMX_MAIL.MAI"   /* in the SYSUAF default directory */
#define MAIL_ID_LEN       16
#define MAIL_FROM_MAX     80
#define MAIL_FIELD_MAX    512               /* To/CC/Subj/body line           */
#define MAIL_DATE_MAX     24                /* "8-OCT-2026 06:37:49.01"       */
#define MAIL_STORE_MAX    1000              /* messages a mail file holds      */

struct mail_store_entry {
    char id[MAIL_ID_LEN + 1];
    char date[MAIL_DATE_MAX];
    char from[MAIL_FROM_MAX];
    char to[MAIL_FIELD_MAX + 1];
    char cc[MAIL_FIELD_MAX + 1];
    char subj[MAIL_FIELD_MAX + 1];
    int  read;
    int  deleted;
};

/*
 * get_user_homedir - a VMS username's SYSUAF default device:[directory].
 * Returns 0, or -1 if the user is not in SYSUAF.
 */
int get_user_homedir(const char *username, char *homedir, size_t sz);

/*
 * mail_store_spec - the VMS filespec of `username`'s mail file
 * ("SYS$SYSROOT:[SYSMGR]OVMX_MAIL.MAI"). Returns 0, or -1 if the user is not
 * in SYSUAF (there is no other place a mailbox could be).
 */
int mail_store_spec(const char *username, char *spec, size_t sz);

/*
 * mail_store_load - read `username`'s committed messages, oldest first, into
 * out[0..max-1] (deleted ones included, flagged). *count gets the number.
 * Returns 0 (an absent mail file is an empty one), or -1 when the user is not
 * in SYSUAF.
 */
int mail_store_load(const char *username, struct mail_store_entry *out, int max,
                    int *count);

/*
 * mail_store_body - call line_cb for each body line of message `id`, in order.
 * Returns the number of lines, or -1 if the mail file cannot be read.
 */
int mail_store_body(const char *username, const char *id,
                    void (*line_cb)(void *ctx, const char *line), void *ctx);

/*
 * mail_count_unread - unread, non-deleted, committed messages for a user (0 on
 * any error). The login-time "You have N new mail messages" count.
 */
int mail_count_unread(const char *username);

/* ---- writers (tools/mail_store.c: MAIL.EXE and MAIL_SERVER.EXE) ---------- */

/*
 * mail_store_deliver - append one message to `recipient`'s mail file. Returns
 * a VMS status: odd only when the whole message, through its committing E
 * record, is on disk; on failure errtext (if given) gets a "%FAC-S-IDENT, text"
 * line for the sender.
 */
uint32_t mail_store_deliver(const char *recipient, const char *from,
                            const char *to, const char *cc, const char *subj,
                            const char *const *lines, unsigned nlines,
                            char *errtext, size_t errcap);

/* mail_store_mark - append a state record ('R' read, 'D' deleted). 0 / -1. */
int mail_store_mark(const char *username, const char *id, char what);

#endif /* VMS_MAIL_NOTIFY_H */
