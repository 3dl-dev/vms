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
    /* A terminal: read one line through the terminal driver -- a prompting
     * read, the host's Start Read prompt given to the driver with it, so the
     * driver puts it on the screen as VMS puts a read's prompt (its leading
     * CR LF is ONE new line, not one more after the echoed RETURN) -- which
     * echoes as asked; `init` (the Start Read's data after its prompt, e.g. a
     * recalled command) starts the line, editable, as TRM$_INISTRNG; timed when `timeout_s` > 0 (the host's Start Read TIMEOUT).
     * Returns the I/O status; on success *n is the line length and *term the
     * terminator character. SS$_TIMEOUT = the time ran out, *n = what was
     * typed before it. SS$_ABORT = the read was cancelled (CTRL/Y): the
     * solicit stays outstanding. */
    uint32_t (*read_line)(void *ctx, const uint8_t *prompt, size_t plen,
                          const uint8_t *init, size_t ilen,
                          uint8_t *buf, size_t cap, int echo,
                          int term_echo, unsigned timeout_s, size_t *n,
                          uint8_t *term);
    /* Not a terminal (a file SYS$INPUT): read the next record. Returns the
     * I/O status (RMS$_EOF / SS$_ENDOFFILE at the end). */
    uint32_t (*read_record)(void *ctx, uint8_t *buf, size_t cap, size_t *n);
    /* (A terminal; may be NULL.) Take the control characters in `mask` (bit
     * n = character n) out of band: the terminal driver hands each one typed
     * to rtpad_session_oob() instead of acting on it (IO$M_OUTBAND). */
    void (*set_oob)(void *ctx, uint32_t mask);
};

/* The control characters a terminal pad always takes from the local driver:
 * CTRL/Y and CTRL/C belong to the REMOTE session, never to the local DCL. */
#define RTPAD_OOB_ALWAYS ((1u << 0x19) | (1u << 0x03))

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
    unsigned rd_timeout;                /* ... and its TIMEOUT, s (0 = none) */
    uint8_t rd_prompt[DNET_CTERM_MAX_DATA]; /* ... and its prompt (a terminal) */
    size_t rd_plen;
    size_t rd_init;                     /* ... of which the tail after END-OF-
                                         * PROMPT is initial input (a recall) */
    /* The host's CHARACTER-ATTRIBUTES per control character (CTERM
     * Characteristics selector 0x0202): bits 0-1 the out-of-band kind
     * (0 none, 1 immediate clear, 2 deferred clear, 3 immediate hello),
     * bit 3 discard output. */
    uint8_t chattr[32];
    uint32_t oob_mask;                  /* what set_oob was last given */
    uint8_t opdu[32];                   /* an Out-of-Band PDU (sent from an AST) */
    unsigned oob_sent, oob_dropped;
    int fresh;                          /* the cursor is where an echoed RETURN left it */
    int done;
    uint32_t status;                    /* the session's ending condition */
    uint8_t cpdu[DNET_CTERM_MAX_PDU];
    uint8_t sense[12];
    int sense_set;
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

/* Must the outstanding read start now, before the user types? A TIMED read
 * (its time runs from the prompt) and one with initial input (a recalled
 * command, shown at once) do; an untimed one, its prompt already shown,
 * waits for the first key so the link is served meanwhile. */
int rtpad_session_read_now(const struct rtpad_session *s);

/* Satisfy the outstanding Start Read from the local side (one line). */
void rtpad_session_input(struct rtpad_session *s);

/* A control character the terminal driver gave out of band (from RTPAD's
 * IO$M_OUTBAND AST). One the host declared out-of-band goes to it as a CTERM
 * Out-of-Band message, echoed as the interrupt when its attributes say so.
 * Any other is dropped: it never reaches the local DCL. */
void rtpad_session_oob(struct rtpad_session *s, uint8_t c);

/* Report THIS terminal (IO$_SENSEMODE's 12-byte buffer) to the host. */
void rtpad_session_set_sense(struct rtpad_session *s, const uint8_t sense[12]);

/* End: Unbind a bound session and say %REM-S-END for `local_node`. */
void rtpad_session_finish(struct rtpad_session *s, const char *local_node);

#endif /* RTPAD_SESSION_H */
