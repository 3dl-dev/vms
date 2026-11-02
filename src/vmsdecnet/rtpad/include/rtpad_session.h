/*
 * rtpad_session.h -- the CTERM terminal side of $ SET HOST, as RTPAD.EXE runs
 * it (rd vms-0a7 S2, vms-18c9).
 *
 * On VMS, SET HOST activates SYS$SYSTEM:RTPAD.EXE, the remote terminal
 * "pad": it opens a logical link to object 42 on the remote node through
 * _NET:, binds a CTERM session, and carries the user's terminal over it until
 * the remote session logs out. This header is the link- and terminal-
 * independent half: the CTERM client protocol decisions (foundation replies,
 * Writes rendered to the screen, one input line per host Start Read, Read
 * Data back, characteristics read, Unbind). The I/O itself is reached through
 * `struct rtpad_io`, which RTPAD.EXE implements with $QIO on a _NET: channel
 * and $QIO on the terminal channel, and which the host tests implement over
 * a replayed real-VAX session.
 *
 * Every byte RTPAD sends or decodes is the vmsdecnet_cterm codec's (pinned to
 * real VAX captures, docs/oracle/vax-sethost-cterm.*); nothing here invents a
 * PDU.
 */
#ifndef RTPAD_SESSION_H
#define RTPAD_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_cterm.h"

struct rtpad_io {
    void *ctx;
    /* Send one CTERM segment on the logical link. Returns a VMS status. */
    uint32_t (*link_send)(void *ctx, const uint8_t *pdu, size_t n);
    /* Put host output on the screen as it is (no carriage control). */
    void (*show)(void *ctx, const uint8_t *b, size_t n);
    /* Write a message as one VMS record (a line of its own). */
    void (*record)(void *ctx, const char *msg);
    /* A terminal: read one line through the terminal driver, which echoes as
     * asked. Returns the I/O status; on success *n is the line length and
     * *term the terminator character. SS$_ABORT = the read was cancelled
     * (CTRL/Y): the solicit stays outstanding. */
    uint32_t (*read_line)(void *ctx, uint8_t *buf, size_t cap, int echo,
                          int term_echo, size_t *n, uint8_t *term);
    /* Not a terminal (a file SYS$INPUT): read the next record. Returns the
     * I/O status (RMS$_EOF / SS$_ENDOFFILE at the end). */
    uint32_t (*read_record)(void *ctx, uint8_t *buf, size_t cap, size_t *n);
};

struct rtpad_session {
    struct dnet_cterm_session term;
    struct dnet_cterm_inq inq;
    const struct rtpad_io *io;
    int is_terminal;                    /* SYS$INPUT is the job's terminal */
    int read_pending;                   /* a host Start Read is outstanding */
    int bound_ever;                     /* a session was established */
    int input_eof;
    int skip_lf;                        /* CTERM Write "newline" state */
    int rd_echo, rd_term_echo;          /* the pending Start Read's echo */
    int done;
    uint32_t status;                    /* the session's ending condition */
    uint8_t cpdu[DNET_CTERM_MAX_PDU];
};

/* Initialise a TERMINAL-role session over `io`. Returns 0, or -1. */
int rtpad_session_init(struct rtpad_session *s, const struct rtpad_io *io,
                       int is_terminal);

/* The logical link is up: arm the CTERM client (the host speaks first). */
void rtpad_session_linkup(struct rtpad_session *s);

/* One NSP data segment from the host. Bounded: any input is safe. */
void rtpad_session_rx(struct rtpad_session *s, const uint8_t *data, size_t len);

/* Does the session want local input now? (A terminal only while a Start Read
 * is outstanding; a file whenever the host solicits.) */
int rtpad_session_wants_input(const struct rtpad_session *s);

/* Satisfy the outstanding Start Read from the local side (one line). */
void rtpad_session_input(struct rtpad_session *s);

/* End: Unbind a bound session and say %REM-S-END for `local_node`. */
void rtpad_session_finish(struct rtpad_session *s, const char *local_node);

#endif /* RTPAD_SESSION_H */
