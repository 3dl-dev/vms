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

    strncpy(term->device_name, "_FTA0:", sizeof(term->device_name) - 1);
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
 * Characteristic display table — maps bit flags to VMS display names.
 *
 * Each entry has an "active" name (shown when bit is set) and
 * an "inactive" name (shown when bit is clear).
 */
static const struct {
    uint32_t    bit;
    const char *active_name;
    const char *inactive_name;
} char_display[] = {
    { TT_SCOPE,        "Scope",           "No Scope"          },
    { TT_ECHO,         "Echo",            "No Echo"           },
    { TT_TYPEAHEAD,    "Type_ahead",      "No Type_ahead"     },
    { TT_HOSTSYNC,     "Hostsync",        "No Hostsync"       },
    { TT_TTSYNC,       "TTsync",          "No TTsync"         },
    { TT_LOWERCASE,    "Lowercase",       "Uppercase"         },
    { TT_TAB,          "Tab",             "No Tab"            },
    { TT_WRAP,         "Wrap",            "No Wrap"           },
    { TT_LINE_EDITING, "Line_Editing",    "No Line_Editing"   },
    { TT_INSERT,       "Insert",          "Overstrike"        },
    { TT_BROADCAST,    "Broadcast",       "No Broadcast"      },
    { TT_EIGHTBIT,     "Eightbit",        "No Eightbit"       },
    { TT_HOLDSCREEN,   "Holdscreen",      "No Holdscreen"     },
    { TT_MECHTAB,      "Mechtab",         "No Mechtab"        },
    { TT_READSYNC,     "Readsync",        "No Readsync"       },
    { TT_PASTHRU,      "Pasthru",         "No Pasthru"        },
    { TT_ESCAPE,       "Escape",          "No Escape"         },
    { TT_FORM,         "Form",            "No Form"           },
    { TT_FULLDUP,      "Fulldup",         "Halfdup"           },
    { TT_MODEM,        "Modem",           "No Modem"          },
    { TT_PAGE,         "Page",            "No Page"           },
    { TT_RUNOUT,       "Runout",          "No Runout"         },
    { TT_FALLBACK,     "Fallback",        "No Fallback"       },
    { TT_DIALUP,       "Dialup",          "No Dialup"         },
    { TT_SECURE,       "Secure",          "No Secure"         },
    { TT_OPER,         "Oper",            "No Oper"           },
    { TT_ALTYPEAHD,    "AltTypeAhd",      "No AltTypeAhd"     },
};

#define CHAR_DISPLAY_COUNT (sizeof(char_display) / sizeof(char_display[0]))

/*
 * vms_terminal_show - Display terminal characteristics in VMS format.
 *
 * Output matches OpenVMS SHOW TERMINAL format:
 *   Terminal: _FTA0:     Device_Type: VT100         Owner: BARON
 *
 *   Terminal Characteristics:
 *     Interactive         Echo               Type_ahead          Hostsync
 *     ...
 *     Width: 132          Page:  48
 */
void vms_terminal_show(const struct vms_terminal *term, FILE *out)
{
    const char *owner = term->owner[0] ? term->owner : "SYSTEM";

    fprintf(out, "Terminal: %-12s Device_Type: %-14s Owner: %s\n\n",
            term->device_name, term->device_type, owner);
    fprintf(out, "Terminal Characteristics:\n");

    /* Always show "Interactive" first (we are always interactive) */
    fprintf(out, "  %-20s", "Interactive");
    int col = 1;

    for (unsigned i = 0; i < CHAR_DISPLAY_COUNT; i++) {
        int set = (term->characteristics & char_display[i].bit) ? 1 : 0;
        const char *name = set ? char_display[i].active_name
                               : char_display[i].inactive_name;
        fprintf(out, "%-20s", name);
        col++;
        if (col >= 4) {
            fprintf(out, "\n  ");
            col = 0;
        }
    }

    /* Finish the line if needed */
    if (col > 0)
        fprintf(out, "\n");

    fprintf(out, "\n  Width: %3d          Page: %3d\n", term->width, term->page);
}

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
        fwrite(&devs[i], sizeof(devs[i]), 1, fp);
    }
    fflush(fp);
}

const char *vms_term_allocate(const char *prefix, pid_t pid, const char *owner)
{
    static char result[16];

    /* Ensure table directory exists */
    FILE *fp = fopen(TERM_TABLE_PATH, "r+b");
    if (!fp) {
        fp = fopen(TERM_TABLE_PATH, "w+b");
        if (!fp) return NULL;
    }
    flock(fileno(fp), LOCK_EX);

    struct terminal_device devs[TERM_TABLE_MAX];
    int count = term_table_load(fp, devs, TERM_TABLE_MAX);

    /* Find next available unit number for the prefix */
    int next_unit = 0;
    for (int i = 0; i < count; i++) {
        /* Extract unit number from existing entries with same prefix */
        if (strncmp(devs[i].name, prefix, strlen(prefix)) == 0) {
            int unit = (int)strtol(devs[i].name + strlen(prefix), NULL, 10);
            if (unit >= next_unit)
                next_unit = unit + 1;
        }
    }

    /* Build the new entry */
    if (count >= TERM_TABLE_MAX) {
        flock(fileno(fp), LOCK_UN);
        fclose(fp);
        return NULL;
    }

    struct terminal_device *nd = &devs[count];
    memset(nd, 0, sizeof(*nd));
    snprintf(nd->name, sizeof(nd->name), "%s%d:", prefix, next_unit);
    nd->owner_pid = pid;
    if (owner) {
        /* Store uppercased owner name */
        size_t i;
        for (i = 0; i < sizeof(nd->owner_name) - 1 && owner[i]; i++)
            nd->owner_name[i] = (char)toupper((unsigned char)owner[i]);
        nd->owner_name[i] = '\0';
    }
    nd->characteristics = TT_DEFAULT_CHARS;
    nd->allocated = 1;
    count++;

    term_table_save(fp, devs, count);

    flock(fileno(fp), LOCK_UN);
    fclose(fp);

    snprintf(result, sizeof(result), "%s", nd->name);
    return result;
}

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

void vms_term_list(struct terminal_device *out_devs, int max, int *count)
{
    *count = 0;

    FILE *fp = fopen(TERM_TABLE_PATH, "rb");
    if (!fp) return;
    flock(fileno(fp), LOCK_SH);

    struct terminal_device devs[TERM_TABLE_MAX];
    int total = term_table_load(fp, devs, TERM_TABLE_MAX);

    flock(fileno(fp), LOCK_UN);
    fclose(fp);

    int n = (total < max) ? total : max;
    for (int i = 0; i < n; i++)
        out_devs[i] = devs[i];
    *count = n;
}
