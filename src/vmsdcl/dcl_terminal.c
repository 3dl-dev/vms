/*
 * dcl_terminal.c - VMS Terminal Characteristics Model
 *
 * Implements the terminal characteristics struct and operations
 * used by SET TERMINAL and SHOW TERMINAL, plus the shared terminal
 * device allocation table for SHOW USERS and SSH session management.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#include "dcl/terminal.h"
#include "ssdef.h"
/* struct vms_procinfo + VMS_PI_V_* valid-bit mask (the executive's $GETJPI
 * contract) -- src/libvmssys/vms_kif.h pulls src/kernel/vms_ioctl.h. */
#include "vms_kif.h"
#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "trmdef.h"

/* Path to the shared terminal device table */
#include "ovmx_layout.h"
#define TERM_TABLE_PATH VMS_TEMP_DIR "/VMS_TERMINALS.DAT"
#define TERM_TABLE_MAX  256

/*
 * vms_terminal_init - Initialize terminal to VMS defaults.
 *
 * Probes the real terminal for width/page if available,
 * otherwise uses 80x24.
 */
void vms_terminal_init(struct vms_terminal *term)
{
    memset(term, 0, sizeof(*term));

    /*
     * NO DEFAULT DEVICE NAME (vms-fb9). This used to seed "_FTA0:", which
     * meant every DCL process on the system claimed the same terminal, and
     * meant the name survived deleting the VMS_TERMINAL handoff and the
     * "_FTA" pool -- the fabrication would simply have moved here. A VMS
     * terminal name identifies a device in the executive's device table
     * (src/kernel/vms_devtab.c); it is looked up, never defaulted. Until
     * DCL can ask the executive which terminal this job is on, there is no
     * name, and device_name stays empty (rule 10: no answer beats a
     * plausible one).
     *
     * device_type keeps its "VT100" default for now: SHOW TERMINAL as a
     * whole is not yet a reader (see the note above cmd_show_terminal in
     * dcl_cmd_show.c), and its characteristic list does not match the
     * oracle either. Changing one field of a display that is wrong as a
     * unit would only make it harder to see that it is wrong. The oracle's
     * answer for an unidentified terminal is "Unknown"
     * (docs/oracle/vax73-terminal-device.md section 3).
     */
    strncpy(term->device_type, "VT100", sizeof(term->device_type) - 1);
    /* owner is set later from context */

    term->characteristics = TT_DEFAULT_CHARS;
    term->width = 80;
    term->page  = 24;
    term->speed = 9600;
    term->parity = 0;  /* none */

    /* Probe real terminal size */
    if (isatty(STDOUT_FILENO)) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_col > 0) term->width = (int)ws.ws_col;
            if (ws.ws_row > 0) term->page  = (int)ws.ws_row;
        }
    }
}

/*
 * vms_terminal_set_char - Set or clear a characteristic bit.
 */
void vms_terminal_set_char(struct vms_terminal *term, uint32_t bit, int on)
{
    if (on)
        term->characteristics |= bit;
    else
        term->characteristics &= ~bit;
}

/*
 * vms_terminal_get_char - Query whether a characteristic is set.
 */
int vms_terminal_get_char(const struct vms_terminal *term, uint32_t bit)
{
    return (term->characteristics & bit) ? 1 : 0;
}

/*
 * vms_terminal_apply - make the characteristics SET TERMINAL changed true of
 * the terminal itself.
 *
 * Echo (and the other characteristics the terminal driver acts on) live in the
 * executive's device row, where the terminal class driver reads them per read
 * (rd vms-f8c): /NOECHO is the driver not echoing, never a substrate termios
 * flag. dcl_tt_set_characteristics() carries them there; this function keeps
 * only the substrate's window geometry in step for full-screen images.
 */
void vms_terminal_apply(const struct vms_terminal *term)
{
    /* Apply width/page to terminal window size */
    if (isatty(STDOUT_FILENO)) {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            ws.ws_col = (unsigned short)term->width;
            ws.ws_row = (unsigned short)term->page;
            ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws);
        }
    }
}

/* ================================================================
 * THE TERMINAL, THROUGH THE EXECUTIVE'S TERMINAL DRIVER (rd vms-f8c)
 *
 * DCL reads its commands the way VMS DCL does: $QIO IO$_READPROMPT on a
 * channel to the terminal. The executive's terminal class driver
 * (src/kernel-core/vms_tt.c) writes the prompt, consumes the type-ahead
 * buffer -- echoing each character as it is consumed, never on receipt --
 * applies the line-editing keys and the terminators, and completes the read
 * with the terminator in the IOSB. Nothing here touches the substrate tty.
 * ================================================================ */
static uint16_t dcl_tt_chan;
static int dcl_tt_state;        /* 0 untried, 1 assigned, -1 no terminal */

static int dcl_tt_assign(void)
{
    static const char ttn[] = "TT:";
    struct dsc$descriptor_s td = { sizeof ttn - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)ttn };

    if (dcl_tt_state == 0)
        dcl_tt_state = (sys$assign(&td, &dcl_tt_chan, 0, NULL, 0) & 1) ? 1 : -1;
    return dcl_tt_state == 1;
}

void dcl_tt_arm_oob(void (*yast)(uint32_t), int y_on, void (*tast)(uint32_t), int t_on)
{
    uint16_t iosb[4];
    /* IO$M_OUTBAND's P2: a quadword, first longword 0, second the mask of
     * control characters -- here CTRL/T (20) */
    static const uint32_t tmask[2] = { 0, 1u << 20 };

    if (!dcl_tt_assign())
        return;
    (void)sys$qiow(0, dcl_tt_chan, IO$_SETMODE | IO$M_CTRLYAST, iosb, NULL, 0,
                   y_on ? (void *)yast : NULL, 0, 3, 0, 0, 0);
    /* the mask's address rides P4: P2 is a longword in this ABI and DCL's
     * data may lie above 4 GB (see sys_qio.c) */
    (void)sys$qiow(0, dcl_tt_chan, IO$_SETMODE | IO$M_OUTBAND, iosb, NULL, 0,
                   t_on ? (void *)tast : NULL, 0, 3, (uintptr_t)tmask, 0, 0);
}

int dcl_tt_read(const char *prompt, size_t prompt_len, char *buf, size_t bufsz,
                uint32_t modifiers, uint32_t timeout_sec, uint16_t *term_out)
{
    uint16_t iosb[4];
    uint32_t func, st;
    size_t n;

    if (term_out)
        *term_out = 0;
    if (!buf || bufsz < 2)
        return DCL_TT_GONE;
    if (!dcl_tt_assign())
        return DCL_TT_NODRIVER;
    /* what DCL has written through stdio reaches the terminal before the
     * prompt does */
    fflush(stdout);
    fflush(stderr);

    static char pbuf[512];
    size_t plen = 0;
    if (prompt && prompt_len) {
        plen = prompt_len;
        if (plen > sizeof pbuf)
            plen = sizeof pbuf;
        memcpy(pbuf, prompt, plen);
    }

    func = (plen ? IO$_READPROMPT : IO$_READVBLK) | modifiers;
    if (timeout_sec)
        func |= IO$M_TIMED;
    memset(iosb, 0, sizeof iosb);
    st = sys$qiow(0, dcl_tt_chan, func, iosb, NULL, 0, buf, (uint32_t)(bufsz - 1),
                  timeout_sec, 0, plen ? (uintptr_t)pbuf : 0, (uint32_t)plen);
    if (st & 1)
        st = iosb[0];
    if (st == SS$_DEVOFFLINE || st == SS$_NOSUCHDEV || st == SS$_IVCHAN ||
        st == SS$_IVDEVNAM)
        return DCL_TT_NODRIVER;
    n = iosb[1] < bufsz - 1 ? iosb[1] : bufsz - 1;
    buf[n] = '\0';
    if (term_out)
        *term_out = iosb[2];
    if (st == SS$_TIMEOUT)
        return DCL_TT_TIMEOUT;
    if (st == SS$_ABORT)
        return DCL_TT_INTR;            /* interrupted (^Y / ^C) */
    if (!(st & 1))
        return DCL_TT_GONE;            /* hangup, or the line is gone */
    if (iosb[3] && iosb[2] == 26 && n == 0)
        return DCL_TT_EOF;             /* ^Z: the driver echoed *EXIT* */
    return (int)n;
}

int dcl_tt_read_ini(const char *prompt, size_t prompt_len, const char *ini,
                    char *buf, size_t bufsz, uint16_t *term_out)
{
    uint16_t iosb[4];
    uint32_t st;
    size_t n;
    static char pbuf[512];
    static char ibuf[512];
    struct ovmx_trm_item items[2];
    size_t plen = prompt_len < sizeof pbuf ? prompt_len : sizeof pbuf;
    size_t ilen = ini ? strlen(ini) : 0;

    if (term_out)
        *term_out = 0;
    if (!buf || bufsz < 2)
        return DCL_TT_GONE;
    if (!dcl_tt_assign())
        return DCL_TT_NODRIVER;
    fflush(stdout);
    fflush(stderr);
    if (ilen > sizeof ibuf)
        ilen = sizeof ibuf;
    memcpy(pbuf, prompt, plen);
    memcpy(ibuf, ini ? ini : "", ilen);
    memset(items, 0, sizeof items);
    items[0].code = TRM$_PROMPT;
    items[0].len = (unsigned short)plen;
    items[0].val = (uintptr_t)pbuf;
    items[1].code = TRM$_INISTRNG;
    items[1].len = (unsigned short)ilen;
    items[1].val = (uintptr_t)ibuf;
    memset(iosb, 0, sizeof iosb);
    st = sys$qiow(0, dcl_tt_chan, IO$_READVBLK | IO$M_EXTEND, iosb, NULL, 0,
                  buf, (uint32_t)(bufsz - 1), 0, 0, (uintptr_t)items,
                  (uint32_t)(ilen ? sizeof items : sizeof items[0]));
    if (st & 1)
        st = iosb[0];
    if (st == SS$_DEVOFFLINE || st == SS$_NOSUCHDEV || st == SS$_IVCHAN ||
        st == SS$_IVDEVNAM)
        return DCL_TT_NODRIVER;
    n = iosb[1] < bufsz - 1 ? iosb[1] : bufsz - 1;
    buf[n] = '\0';
    if (term_out)
        *term_out = iosb[2];
    if (st == SS$_TIMEOUT)
        return DCL_TT_TIMEOUT;
    if (st == SS$_ABORT)
        return DCL_TT_INTR;
    if (!(st & 1))
        return DCL_TT_GONE;
    if (iosb[3] && iosb[2] == 26 && n == 0)
        return DCL_TT_EOF;
    return (int)n;
}

/*
 * dcl_tt_read_line - one line for INQUIRE / READ SYS$INPUT / READ /PROMPT
 * from an interactive terminal: through the terminal driver when stdin is
 * that terminal, else (a pipe, a file, or no driver) the plain stream. 0 with
 * buf filled, -1 at end of file (^Z, or the stream's EOF).
 */
int dcl_tt_read_line(const char *prompt, char *buf, size_t bufsz)
{
    size_t len;

    if (isatty(STDIN_FILENO)) {
        int n = dcl_tt_read(prompt, prompt ? strlen(prompt) : 0, buf, bufsz, 0, 0, NULL);
        if (n >= 0)
            return 0;
        if (n != DCL_TT_NODRIVER)
            return -1;
    }
    if (prompt && prompt[0]) {
        fputs(prompt, stdout);
        fflush(stdout);
    }
    if (!fgets(buf, (int)bufsz, stdin))
        return -1;
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n')
        buf[len - 1] = '\0';
    return 0;
}

/*
 * dcl_tt_set_characteristics - SET TERMINAL's characteristics, in the
 * executive's device row (IO$_SENSEMODE, then IO$_SETMODE with the
 * characteristics buffer -- rd vms-d900's path), so the terminal driver acts
 * on them. `set`/`clr` are VMS_TTC_* bits within the buffer's first three
 * characteristic bytes. SS$ status.
 */
uint32_t dcl_tt_set_characteristics(uint64_t set, uint64_t clr)
{
    uint8_t cb[12];
    uint16_t iosb[4];
    uint64_t chars;
    uint32_t st;

    if (!dcl_tt_assign())
        return SS$_NOSUCHDEV;
    memset(cb, 0, sizeof cb);
    st = sys$qiow(0, dcl_tt_chan, IO$_SENSEMODE, iosb, NULL, 0, cb, sizeof cb, 0, 0, 0, 0);
    if (!(st & 1) || !(iosb[0] & 1))
        return (st & 1) ? iosb[0] : st;
    chars = (uint64_t)cb[4] | ((uint64_t)cb[5] << 8) | ((uint64_t)cb[6] << 16);
    chars = (chars | set) & ~clr;
    cb[4] = (uint8_t)(chars & 0xFF);
    cb[5] = (uint8_t)((chars >> 8) & 0xFF);
    cb[6] = (uint8_t)((chars >> 16) & 0xFF);
    st = sys$qiow(0, dcl_tt_chan, IO$_SETMODE, iosb, NULL, 0, cb, sizeof cb, 0, 0, 0, 0);
    if (!(st & 1))
        return st;
    return iosb[0];
}

/*
 * DELETED, NOT REPLACED (vms-d0b): char_display[] and vms_terminal_show().
 *
 * They were the renderer behind SHOW TERMINAL, and SHOW TERMINAL is now a
 * reader of the executive (src/vmsdcl/dcl_cmd_show.c: $GETJPI for which
 * terminal this job is on, then $GETDVI for that device's row). That left
 * this pair with no caller at all, and a caller-less renderer would not have
 * been harmless: char_display[] invented seven characteristics VMS V7.3 does
 * not print (Scope, Holdscreen, Mechtab, Oper, Page, Runout, AltTypeAhd)
 * while omitting most of the names it does, and printed Width and Page in a
 * layout the oracle does not use. Keeping a second, wrong renderer of VMS
 * output in the tree is how it comes back. The oracle's real list, and the
 * only renderer of it, live at the reader -- see terminal_chars[] in
 * dcl_cmd_show.c and docs/oracle/vax73-terminal-device.md section 2.
 *
 * What is NOT deleted, because it is still used: struct vms_terminal, the
 * TT_* bits, vms_terminal_init(), vms_terminal_set_char(),
 * vms_terminal_get_char() and vms_terminal_apply(). SET TERMINAL still writes
 * them and vms_terminal_apply() still reaches the real termios. The TT_* bits
 * remain OVMX-defined and are still labelled as such in
 * src/vmsdcl/include/dcl/terminal.h (vms-2cb).
 */

/* ------------------------------------------------------------------ */
/* Terminal Device Allocation Table                                    */
/*                                                                     */
/* File-based shared table at /tmp/vms_terminals.dat.  Each entry is   */
/* a fixed-size terminal_device struct.  File locking (flock) ensures   */
/* concurrent access from vmssshd + vmsdcl is safe.                    */
/* ------------------------------------------------------------------ */

/* Check if a PID is still alive */
static int pid_alive(pid_t pid)
{
    if (pid <= 0) return 0;
    return (kill(pid, 0) == 0 || errno == EPERM);
}

/*
 * Load the table, cleaning stale entries (dead PIDs).
 * Returns number of valid entries.  Caller must fclose(fp).
 */
static int term_table_load(FILE *fp, struct terminal_device *devs, int max)
{
    int count = 0;
    rewind(fp);
    struct terminal_device d;
    while (count < max && fread(&d, sizeof(d), 1, fp) == 1) {
        if (d.allocated && pid_alive(d.owner_pid)) {
            devs[count++] = d;
        }
        /* skip stale entries — they'll be pruned on rewrite */
    }
    return count;
}

/* Rewrite the table with only valid entries */
static void term_table_save(FILE *fp, const struct terminal_device *devs, int count)
{
    rewind(fp);
    if (ftruncate(fileno(fp), 0) < 0) {
        /* non-fatal */
    }
    for (int i = 0; i < count; i++) {
        if (fwrite(&devs[i], sizeof(devs[i]), 1, fp) != 1) {
            /* non-fatal: best-effort table save */
            break;
        }
    }
    fflush(fp);
}

/*
 * vms_term_allocate() WAS HERE AND IS DELETED (vms-fb9, round 2).
 *
 * It minted this process a terminal-device name out of the /tmp table
 * above -- "_FTA0:", then "_FTA1:", ... -- and every reader that wanted to
 * know "what terminal am I on" was answered from it. A process choosing
 * its own device name is the same shape as the VMS_PRCNAM environment
 * cheat the operator rejected on 2026-07-30: a name nothing else in the
 * system agrees with. Its last production caller (src/vmsdcl/dcl_main.c)
 * went when the VMS_TERMINAL handoff went, leaving a name generator with
 * no user; under rule 10 a mechanism for a condition OVMX no longer has is
 * deleted, not kept behind a lint. See src/vmsdcl/include/dcl/terminal.h.
 *
 * The remover below stays because it still has a caller (dcl_main.c, at
 * logout); with the allocator gone the table can no longer gain an entry,
 * so what it removes is always nothing. That is the honest state, not a
 * bug to "fix" by putting the allocator back.
 *
 * vms_term_list() -- THE READER -- WAS ALSO HERE AND IS ALSO DELETED
 * (vms-72c). It had exactly the same "always sees an empty table" problem
 * as the allocator's removal left behind, and its one caller
 * (cmd_show_users() in src/vmsdcl/dcl_cmd_show.c) took that permanent
 * emptiness as license to fabricate a single row about the CALLING
 * process instead -- the same self-reporting shape Rule 10's worked
 * examples reject, reproduced one layer up from where vms-fb9 already
 * deleted it once. SHOW USERS now reads src/kernel/vms_proctab.c through
 * vms_kif_procscan() directly, the same executive-resident source
 * cmd_show_system() and cmd_show_process() already use, so this reader
 * of a table that can never be written is deleted rather than kept
 * behind a lint -- the same rule vms_term_allocate()'s deletion states
 * above, applied to the half of the pair that survived it.
 */

void vms_term_deallocate(const char *device_name)
{
    if (!device_name) return;

    FILE *fp = fopen(TERM_TABLE_PATH, "r+b");
    if (!fp) return;
    flock(fileno(fp), LOCK_EX);

    struct terminal_device devs[TERM_TABLE_MAX];
    int count = term_table_load(fp, devs, TERM_TABLE_MAX);

    /* Remove the matching entry */
    int new_count = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(devs[i].name, device_name) != 0) {
            if (i != new_count)
                devs[new_count] = devs[i];
            new_count++;
        }
    }

    term_table_save(fp, devs, new_count);

    flock(fileno(fp), LOCK_UN);
    fclose(fp);
}

/*
 * dcl_format_ctrl_t_status - render the reflexive Ctrl/T status line.
 *
 * FORMAT (CLEAN-ROOM, Rule 8). The one-line layout and every field are from
 * PUBLIC OpenVMS documentation -- the OpenVMS User's Manual, "Interrupting
 * Command Execution" / "Ctrl/T", which documents:
 *
 *     GREEN::MCCARTHY  13:45:02 EVE    CPU=00:00:03.33 PF=778 IO=295 MEM=315
 *
 * and defines the fields as: node and user name; current time of day; the
 * name of the image (program) executing; charged CPU time in
 * hours:minutes:seconds.hundredths; the accumulated page-fault count; the
 * level of I/O activity; and memory usage listed in CPU-specific pages.
 * (No VSI/HPE binary was disassembled; format taken from the manual text.)
 *
 * DATA SOURCE (INV-6 / Rule 9 / Rule 11). Every measured field is READ from
 * the executive's $GETJPI row (struct vms_procinfo) the caller already
 * fetched with vms_kif_getjpi_self() -- the SAME source SHOW PROCESS reads
 * (src/vmsdcl/dcl_cmd_show.c). This function fabricates NOTHING: a field
 * whose fields_valid bit is CLEAR is OMITTED, never printed as a plausible
 * zero.
 *
 *   node   caller-supplied (ovmx_node_name() -> SCSNODE, the VMS node
 *          identity, NOT the Linux hostname -- INV-4).
 *   user   info->username (executive-stamped identity, "" if none).
 *   time   now, formatted as local HH:MM:SS.
 *   image  caller-supplied JPI$_IMAGNAME. At the DCL prompt no image is
 *          activated, so this is empty and the field collapses -- which is
 *          what VMS shows there (the field names the executing image, and
 *          none is). OVMX does not yet source JPI$_IMAGNAME while an image
 *          runs; see the gap note in dcl_main.c's Ctrl-T handler.
 *   CPU    info->cputim (JPI$_CPUTIM, 10ms units) if VMS_PI_V_CPUTIM.
 *   PF     info->pageflts (JPI$_PAGEFLTS) if VMS_PI_V_PAGEFLTS.
 *   IO     JPI$_DIRIO+JPI$_BUFIO. OVMX's executive carries these
 *          STRUCTURAL, valid bit CLEAR (no faithful direct/buffered I/O
 *          split exists -- src/kernel/vms_ioctl.h), so the field is OMITTED
 *          rather than fabricated (INV-6). Sourcing it is deferred work.
 *   MEM    info->pages (JPI$_PPGCNT, resident pages) if VMS_PI_V_PAGES.
 *
 * Returns SS$_NORMAL on success, SS$_BADPARAM on a bad argument. The line is
 * written WITHOUT a trailing newline; the caller frames it.
 */
uint32_t dcl_format_ctrl_t_status(const struct vms_procinfo *info,
                                  const char *node,
                                  const char *image,
                                  time_t now,
                                  char *out, size_t outlen)
{
    if (!info || !out || outlen == 0)
        return SS$_BADPARAM;

    const char *nodestr = (node && node[0]) ? node : "";
    const char *userstr = info->username;   /* executive identity; may be "" */

    struct tm tmv;
    localtime_r(&now, &tmv);

    /* Leading identity + time + (optional) image, VMS spacing. */
    /* "VAX1::SYSTEM 19:41:08   (DCL)   CPU=..." -- one blank after the user
     * name (VAX V7.3 keystroke OOB.PROMPT T2; rd vms-bd71) */
    int n = snprintf(out, outlen, "%s::%s %02d:%02d:%02d   ",
                     nodestr, userstr,
                     tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    if (n < 0 || (size_t)n >= outlen)
        return SS$_BADPARAM;
    size_t len = (size_t)n;

    if (image && image[0]) {
        n = snprintf(out + len, outlen - len, "%s   ", image);
        if (n < 0 || (size_t)n >= outlen - len)
            return SS$_BADPARAM;
        len += (size_t)n;
    }

    /* CPU=hh:mm:ss.cc -- JPI$_CPUTIM is in 10ms units (centiseconds). */
    if (info->fields_valid & VMS_PI_V_CPUTIM) {
        unsigned long cs = info->cputim;
        unsigned long total_sec = cs / 100UL;
        unsigned long cc = cs % 100UL;
        unsigned long hh = total_sec / 3600UL;
        unsigned long mm = (total_sec % 3600UL) / 60UL;
        unsigned long ss = total_sec % 60UL;
        n = snprintf(out + len, outlen - len,
                     "CPU=%02lu:%02lu:%02lu.%02lu", hh, mm, ss, cc);
        if (n < 0 || (size_t)n >= outlen - len)
            return SS$_BADPARAM;
        len += (size_t)n;
    }

    if (info->fields_valid & VMS_PI_V_PAGEFLTS) {
        n = snprintf(out + len, outlen - len, " PF=%" PRIu32, info->pageflts);
        if (n < 0 || (size_t)n >= outlen - len)
            return SS$_BADPARAM;
        len += (size_t)n;
    }

    /* IO = JPI$_DIRIO + JPI$_BUFIO, the executive's own counts (rd
     * vms-bd71); shown only when the executive says they are sourced. */
    if (info->fields_valid & (VMS_PI_V_DIRIO | VMS_PI_V_BUFIO)) {
        n = snprintf(out + len, outlen - len, " IO=%" PRIu32,
                     info->dirio + info->bufio);
        if (n < 0 || (size_t)n >= outlen - len)
            return SS$_BADPARAM;
        len += (size_t)n;
    }

    if (info->fields_valid & VMS_PI_V_PAGES) {
        n = snprintf(out + len, outlen - len, " MEM=%" PRIu32, info->pages);
        if (n < 0 || (size_t)n >= outlen - len)
            return SS$_BADPARAM;
        len += (size_t)n;
    }

    return SS$_NORMAL;
}

