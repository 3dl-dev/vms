/*
 * dcl_terminal.c - VMS Terminal Characteristics Model
 *
 * Implements the terminal characteristics struct and operations
 * used by SET TERMINAL and SHOW TERMINAL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "dcl/terminal.h"

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
