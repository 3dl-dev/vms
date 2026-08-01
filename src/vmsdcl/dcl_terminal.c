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
#include <termios.h>
#include <ctype.h>

#include "dcl/terminal.h"

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
 * vms_terminal_apply - Apply characteristics to real termios/winsize.
 *
 * Only characteristics with a real mapping are applied:
 *   TT_ECHO     → termios ECHO flag
 *   width/page  → ioctl TIOCSWINSZ
 * All others are stored and reported but have no physical effect.
 */
void vms_terminal_apply(const struct vms_terminal *term)
{
    /* Apply echo setting via termios */
    if (isatty(STDIN_FILENO)) {
        struct termios tio;
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {
            if (term->characteristics & TT_ECHO)
                tio.c_lflag |= ECHO;
            else
                tio.c_lflag &= ~(tcflag_t)ECHO;
            tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }
    }

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

