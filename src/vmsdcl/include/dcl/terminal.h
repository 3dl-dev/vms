/*
 * terminal.h - VMS Terminal Characteristics Model
 *
 * Provides a structured representation of VMS terminal characteristics
 * that can be queried and modified via SET TERMINAL / SHOW TERMINAL.
 * Characteristics that map to real termios settings are applied;
 * others are stored and reported but have no physical effect.
 */

#ifndef __DCL_TERMINAL_H
#define __DCL_TERMINAL_H

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * Terminal characteristic bits.
 *
 * THESE ARE OVMX-DEFINED VALUES, NOT VMS $TTDEF TT$ CONSTANTS. The
 * line that used to be here claimed they matched VMS's; they do not.
 * Neither the bit positions nor, in several cases, the characteristic
 * NAMES correspond to OpenVMS: the display table in
 * src/vmsdcl/dcl_terminal.c carries Scope, Holdscreen, Mechtab, Oper,
 * Page, Runout and AltTypeAhd, none of which appear in the SHOW
 * TERMINAL output of OpenVMS VAX V7.3, and it omits most of the names
 * that do (docs/oracle/vax73-terminal-device.md section 2 has the real
 * list, captured verbatim from the ~/vax lab).
 *
 * CLAUDE.md rule 8 permits OVMX to define its own representation where
 * the public documentation publishes no byte-level layout -- which is
 * the case for $TTDEF -- but requires it to be LABELLED as an OVMX
 * design choice rather than presented as VMS-authentic. This comment
 * is that label. Nothing may assume these values or names match VMS.
 *
 * The executive's own characteristic vector (VMS_TTC_* in
 * src/kernel/vms_ioctl.h) is the oracle-pinned one: its NAMES are
 * exactly the V7.3 set, with only the bit positions OVMX's own.
 * Reconciling this header and dcl_terminal.c's char_display[] against
 * that vector is tracked separately -- it is a change to user-visible
 * SET/SHOW TERMINAL output, not a comment fix.
 */
#define TT_ECHO          (1U << 0)
#define TT_WRAP          (1U << 1)
#define TT_BROADCAST     (1U << 2)
#define TT_TYPEAHEAD     (1U << 3)
#define TT_HOSTSYNC      (1U << 4)
#define TT_TTSYNC        (1U << 5)
#define TT_LINE_EDITING  (1U << 6)
#define TT_INSERT        (1U << 7)
#define TT_SCOPE         (1U << 8)
#define TT_LOWERCASE     (1U << 9)
#define TT_TAB           (1U << 10)
#define TT_MECHTAB       (1U << 11)
#define TT_WRAP_AT_EOL   (1U << 12)
#define TT_HOLDSCREEN    (1U << 13)
#define TT_EIGHTBIT      (1U << 14)
#define TT_NOBRDCST      (1U << 15)   /* inverse of BROADCAST for display */
#define TT_READSYNC      (1U << 16)
#define TT_PASTHRU       (1U << 17)
#define TT_NOECHO        (1U << 18)   /* inverse of ECHO for display */
#define TT_ESCAPE        (1U << 19)
#define TT_NOTYPEAHEAD   (1U << 20)   /* inverse of TYPEAHEAD for display */
#define TT_FORM          (1U << 21)
#define TT_FULLDUP       (1U << 22)
#define TT_MODEM         (1U << 23)
#define TT_OPER          (1U << 24)
#define TT_PAGE          (1U << 25)
#define TT_ALTYPEAHD     (1U << 26)
#define TT_RUNOUT        (1U << 27)   /* physical carriages */
#define TT_FALLBACK      (1U << 28)
#define TT_DIALUP        (1U << 29)
#define TT_SECURE        (1U << 30)

/* Default characteristics for a VT100-class terminal */
#define TT_DEFAULT_CHARS ( \
    TT_ECHO | TT_WRAP | TT_BROADCAST | TT_TYPEAHEAD | \
    TT_HOSTSYNC | TT_TTSYNC | TT_LINE_EDITING | TT_INSERT | \
    TT_SCOPE | TT_LOWERCASE | TT_TAB | TT_FULLDUP | TT_PAGE)

/* VMS terminal state */
struct vms_terminal {
    char device_name[16];      /* _FTA0: */
    char device_type[16];      /* VT100 */
    char owner[64];
    uint32_t characteristics;  /* bit field of TT_ flags */
    int width;
    int page;
    int speed;                 /* baud rate (display only) */
    int parity;                /* 0=none, 1=even, 2=odd */
};

/* Initialize terminal to VMS defaults, probing real terminal */
void vms_terminal_init(struct vms_terminal *term);

/* Set or clear a characteristic bit */
void vms_terminal_set_char(struct vms_terminal *term, uint32_t bit, int on);

/* Query a characteristic bit (returns nonzero if set) */
int vms_terminal_get_char(const struct vms_terminal *term, uint32_t bit);

/* Apply characteristics that map to real termios (echo, etc.) */
void vms_terminal_apply(const struct vms_terminal *term);

/* vms_terminal_show() was here and is DELETED (vms-d0b). SHOW TERMINAL reads
 * the executive now; see the note at the deletion site in dcl_terminal.c. */

/* ---- Terminal Device Allocation Table ---- */

#define VMS_TERM_MAX_DEVICES 100
#define VMS_TERM_TABLE_PATH "/tmp/vms_terminals.dat"

struct terminal_device {
    char     name[16];
    pid_t    owner_pid;
    char     owner_name[64];
    uint32_t characteristics;
    int      allocated;       /* 1 = in use */
};

/*
 * THE ALLOCATOR IS DELETED (vms-fb9, round 2). vms_term_allocate() minted
 * a device name for a process out of a private /tmp file -- "_FTA0:",
 * "_FTA1:", ... -- and handed it back as if it were the terminal that
 * process was on. That is a process naming its own device, which is the
 * facade this item exists to remove (CLAUDE.md rule 11: a device is
 * executive-resident, and a user-visible command READS it). Its last
 * production caller went with the environment handoff; keeping the
 * mechanism afterwards would be keeping a machine for a condition OVMX no
 * longer has (rule 10), and a lint that merely forbade CALLING it from one
 * file was proven evadable by moving the call to another file.
 *
 * tests/integration/test_terminal_identity.sh now asserts the SYMBOL is
 * absent from the whole tree, definition included -- so re-adding it
 * anywhere is what goes red, not re-adding a call in one named file.
 *
 * What remained below used to be the table's READ and REMOVE halves,
 * which had callers (SHOW USERS, DCL exit) that could only ever observe
 * or clear an empty table. SHOW USERS's converted to the executive
 * process table is vms-72c's, done: vms_term_list() -- the READ half --
 * is deleted along with it, for the identical reason vms_term_allocate()
 * above was: a mechanism whose one caller is gone and whose only
 * possible answer was "nothing" is not kept behind a lint. Only the
 * REMOVE half, vms_term_deallocate(), remains, because src/vmsdcl/
 * dcl_main.c still calls it at logout; converting or deleting that
 * caller is not this item's scope.
 */
void vms_term_deallocate(const char *device_name);

/*
 * dcl_format_ctrl_t_status - render the reflexive Ctrl/T status line into
 * `out` from an executive $GETJPI row. Pure function (no I/O, no executive
 * call): the caller supplies the vms_procinfo it already read with
 * vms_kif_getjpi_self(), the VMS node name, the executing image name (""
 * when none), and the wall-clock time. See the definition in
 * dcl_terminal.c for the format citation and per-field data sourcing.
 * Returns SS$_NORMAL on success. `info` is const void* so this header
 * does not have to pull the kernel ioctl structures.
 */
#include <time.h>
struct vms_procinfo;
uint32_t dcl_format_ctrl_t_status(const struct vms_procinfo *info,
                                  const char *node,
                                  const char *image,
                                  time_t now,
                                  char *out, size_t outlen);

#endif /* __DCL_TERMINAL_H */
