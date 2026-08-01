/*
 * dcl_cmd_set.c - DCL SET command implementations
 *
 * All cmd_set_* functions and the SET dispatcher.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>
#include <termios.h>
#include <fcntl.h>

#include "dcl/context.h"
#include "dcl/terminal.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/dcl_cmd.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "starlet.h"
#include "vmsfs/filespec.h"
#include "vms_kif.h"

/* Forward declarations for queue subcommands (dcl_cmd_process.c) */
extern int cmd_set_entry(struct dcl_command *cmd);
extern int cmd_set_queue(struct dcl_command *cmd);

/*
 * enforced_privs_held - the SAME source and the SAME mask every reporting
 * surface reads (dcl_lexical.c's F$PRIVILEGE, dcl_cmd_show.c's SHOW
 * PROCESS/PRIVILEGES): a fresh executive read, intersected with
 * VMS_PRV_M_ENFORCED (src/kernel/vms_ioctl.h).
 *
 * WHY THIS EXISTS (vms-2b8 round 5). Every privilege GATE below used to
 * read ctx->privileges directly -- the RAW, unmasked cur_privs this
 * session's identity was given at VMS_IOCTL_SETIDENT time. For an
 * identity established with SETPRV (e.g. the SYSTEM account, whose
 * SYSUAF record authorizes ALL privileges and whose OVMX DESIGN CHOICE
 * sets cur_privs = authorized_privs verbatim -- see vms_proctab.c's
 * vms_ioctl_setident), that raw mask genuinely contains bits such as
 * ALTPRI, SYSPRV and BYPASS that VMS_PRV_M_ENFORCED excludes because
 * nothing in this tree actually gates on them yet (vms_kif_setprv is
 * OVMX-UNWIRED pending vms-pv1).
 *
 * MEASURED on the real QEMU runtime: with that raw read, F$PRIVILEGE
 * ("ALTPRI") answered FALSE (it already masks to VMS_PRV_M_ENFORCED, per
 * the comment on lex_privilege()) while SET PROCESS/PRIORITY=6 in the
 * SAME session, SAME instant, was AUTHORIZED -- the gate saw ALTPRI in
 * the raw mask and granted the very operation the reporting surface had
 * just said this process could not do. A process that checks
 * F$PRIVILEGE before attempting the operation, exactly as VMS programs
 * are written to, would have made the wrong decision.
 *
 * THE FIX IS ONE SOURCE OF TRUTH, not a gate-specific patch: every gate
 * in this file now asks this same function, which asks the executive
 * fresh (never a stale ctx->privileges snapshot) and applies the exact
 * mask the reporting surfaces apply. A privilege absent from
 * VMS_PRV_M_ENFORCED can no longer authorize anything here, no matter
 * what the identity's raw authorized mask contains -- which is Rule 10's
 * HIDE answer applied to the gates, not just the display: OVMX does not
 * yet enforce ALTPRI/SYSPRV/BYPASS/OPER anywhere, so nothing may be
 * granted on the strength of holding them until vms-pv1 wires real
 * enforcement in.
 *
 * Fails closed: a read that cannot reach the executive returns 0 (holds
 * nothing), the same fail-closed choice lex_privilege() makes.
 */
static uint64_t enforced_privs_held(void)
{
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t jst = vms_kif_getjpi_self(&info);
    if (!(jst & 1))
        return 0;
    return info.cur_privs & VMS_PRV_M_ENFORCED;
}

static int cmd_set_default(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NODIR", "missing directory specification");
        return SS$_BADPARAM;
    }

    const char *dirspec = cmd->params[1];
    char linux_path[1024];

    dcl_resolve_path(ctx, dirspec, linux_path, sizeof(linux_path));

    /* Remove trailing slash for stat */
    char check_path[1024];
    strncpy(check_path, linux_path, sizeof(check_path) - 1);
    check_path[sizeof(check_path) - 1] = '\0';
    size_t cplen = strlen(check_path);
    if (cplen > 1 && check_path[cplen - 1] == '/') {
        check_path[cplen - 1] = '\0';
    }

    struct stat st;
    if (stat(check_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        dcl_error("DCL", 2, "DIRECT", "invalid directory - \\%s\\", dirspec);
        return SS$_NOSUCHFILE;
    }

    /* Store the VMS dirspec directly — don't round-trip through Linux.
     * If the spec has a device/logical, use as-is.
     * If relative ([DIR]), prepend the current default's device. */
    if (strchr(dirspec, ':')) {
        /* Full spec with device/logical — store directly */
        strncpy(ctx->default_dir, dirspec, sizeof(ctx->default_dir) - 1);
        ctx->default_dir[sizeof(ctx->default_dir) - 1] = '\0';
    } else if (dirspec[0] == '[') {
        /* Relative spec — prepend current device */
        char device[128] = "";
        const char *colon = strchr(ctx->default_dir, ':');
        if (colon) {
            size_t dlen = (size_t)(colon - ctx->default_dir);
            if (dlen < sizeof(device)) {
                memcpy(device, ctx->default_dir, dlen);
                device[dlen] = '\0';
            }
        }
        if (device[0])
            snprintf(ctx->default_dir, sizeof(ctx->default_dir),
                     "%s:%s", device, dirspec);
        else
            strncpy(ctx->default_dir, dirspec, sizeof(ctx->default_dir) - 1);
    } else {
        /* Bare name — treat as logical or directory */
        strncpy(ctx->default_dir, dirspec, sizeof(ctx->default_dir) - 1);
        ctx->default_dir[sizeof(ctx->default_dir) - 1] = '\0';
    }

    /* Change the process working directory too */
    if (chdir(check_path) != 0) {
        /* Non-fatal - VMS default and Linux CWD diverge */
    }

    return SS$_NORMAL;
}

/*
 * SET PROMPT - Change the interactive prompt.
 */
static int cmd_set_prompt(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOKEYW", "missing prompt string");
        return SS$_BADPARAM;
    }

    strncpy(ctx->prompt, cmd->params[1], sizeof(ctx->prompt) - 1);
    ctx->prompt[sizeof(ctx->prompt) - 1] = '\0';

    return SS$_NORMAL;
}

/*
 * SET VERIFY / SET NOVERIFY
 */
static int cmd_set_verify(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count >= 1) {
        if (strcasecmp(cmd->params[0], "VERIFY") == 0) {
            ctx->verify = 1;
        } else if (strcasecmp(cmd->params[0], "NOVERIFY") == 0) {
            ctx->verify = 0;
        }
    }

    return SS$_NORMAL;
}

/*
 * SET TERMINAL - Modify terminal characteristics.
 *
 * SET TERMINAL /WIDTH=n /PAGE=n /ECHO /NOECHO /WRAP /NOWRAP
 *              /INSERT /OVERSTRIKE /BROADCAST /NOBROADCAST
 *              /LINE_EDITING /NOLINE_EDITING /DEVICE_TYPE=type
 *              /HOSTSYNC /NOHOSTSYNC /TTSYNC /NOTTSYNC
 *              /TYPEAHEAD /NOTYPEAHEAD /TAB /NOTAB
 *              /SCOPE /NOSCOPE /LOWERCASE /UPPERCASE
 *              /HOLDSCREEN /NOHOLDSCREEN /EIGHTBIT /NOEIGHTBIT
 *              /READSYNC /NOREADSYNC /PASTHRU /NOPASTHRU
 *              /ESCAPE /NOESCAPE /FORM /NOFORM
 *              /FULLDUP /HALFDUP /MODEM /NOMODEM
 *              /PAGE_CHAR /NOPAGE_CHAR /SECURE /NOSECURE
 *              /FALLBACK /NOFALLBACK /SPEED=n /PARITY=type
 *
 * Stores settings in the vms_terminal model and applies those
 * that map to real termios / ioctl.
 */
static int cmd_set_terminal(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();
    struct vms_terminal *term = &ctx->terminal;
    int changed = 0;

    /* /WIDTH=n */
    const char *width_val = dcl_qualifier_value(cmd, "WIDTH");
    if (width_val && *width_val) {
        char *endp;
        int w = (int)strtol(width_val, &endp, 10);
        if (endp == width_val || *endp != '\0' || w < 1 || w > 32767) {
            dcl_error("SET", 2, "INVWIDTH",
                      "invalid terminal width - \\%s\\", width_val);
            return SS$_BADPARAM;
        }
        term->width = w;
        changed = 1;
    }

    /* /PAGE=n */
    const char *page_val = dcl_qualifier_value(cmd, "PAGE");
    if (page_val && *page_val) {
        char *endp;
        int p = (int)strtol(page_val, &endp, 10);
        if (endp == page_val || *endp != '\0' || p < 0 || p > 32767) {
            dcl_error("SET", 2, "INVPAGE",
                      "invalid terminal page length - \\%s\\", page_val);
            return SS$_BADPARAM;
        }
        term->page = p;
        changed = 1;
    }

    /* /SPEED=n */
    const char *speed_val = dcl_qualifier_value(cmd, "SPEED");
    if (speed_val && *speed_val) {
        char *endp;
        int s = (int)strtol(speed_val, &endp, 10);
        if (endp == speed_val || *endp != '\0' || s < 0) {
            dcl_error("SET", 2, "INVSPEED",
                      "invalid terminal speed - \\%s\\", speed_val);
            return SS$_BADPARAM;
        }
        term->speed = s;
        changed = 1;
    }

    /* /PARITY=type (NONE, EVEN, ODD) */
    const char *parity_val = dcl_qualifier_value(cmd, "PARITY");
    if (parity_val && *parity_val) {
        if (strncasecmp(parity_val, "NONE", 4) == 0)
            term->parity = 0;
        else if (strncasecmp(parity_val, "EVEN", 4) == 0)
            term->parity = 1;
        else if (strncasecmp(parity_val, "ODD", 3) == 0)
            term->parity = 2;
        else {
            dcl_error("SET", 2, "INVPAR",
                      "invalid parity type - \\%s\\", parity_val);
            return SS$_BADPARAM;
        }
        changed = 1;
    }

    /* /DEVICE_TYPE=type */
    const char *devtype_val = dcl_qualifier_value(cmd, "DEVICE_TYPE");
    if (!devtype_val) devtype_val = dcl_qualifier_value(cmd, "DEVICE");
    if (devtype_val && *devtype_val) {
        strncpy(term->device_type, devtype_val, sizeof(term->device_type) - 1);
        term->device_type[sizeof(term->device_type) - 1] = '\0';
        /* Uppercase the device type */
        for (char *p = term->device_type; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        changed = 1;
    }

    /*
     * Boolean characteristic qualifiers.
     * Each pair: /NAME sets bit, /NONAME clears bit.
     * Check NO-form first so that if both are present, the positive wins.
     */
    static const struct { const char *on; const char *off; uint32_t bit; } quals[] = {
        { "ECHO",          "NOECHO",          TT_ECHO          },
        { "WRAP",          "NOWRAP",          TT_WRAP          },
        { "BROADCAST",     "NOBROADCAST",     TT_BROADCAST     },
        { "TYPEAHEAD",     "NOTYPEAHEAD",     TT_TYPEAHEAD     },
        { "HOSTSYNC",      "NOHOSTSYNC",      TT_HOSTSYNC      },
        { "TTSYNC",        "NOTTSYNC",        TT_TTSYNC        },
        { "LINE_EDITING",  "NOLINE_EDITING",  TT_LINE_EDITING  },
        { "INSERT",        "OVERSTRIKE",      TT_INSERT        },
        { "SCOPE",         "NOSCOPE",         TT_SCOPE         },
        { "LOWERCASE",     "UPPERCASE",       TT_LOWERCASE     },
        { "TAB",           "NOTAB",           TT_TAB           },
        { "MECHTAB",       "NOMECHTAB",       TT_MECHTAB       },
        { "HOLDSCREEN",    "NOHOLDSCREEN",    TT_HOLDSCREEN    },
        { "EIGHTBIT",      "NOEIGHTBIT",      TT_EIGHTBIT      },
        { "READSYNC",      "NOREADSYNC",      TT_READSYNC      },
        { "PASTHRU",       "NOPASTHRU",       TT_PASTHRU       },
        { "ESCAPE",        "NOESCAPE",        TT_ESCAPE        },
        { "FORM",          "NOFORM",          TT_FORM          },
        { "FULLDUP",       "HALFDUP",         TT_FULLDUP       },
        { "MODEM",         "NOMODEM",         TT_MODEM         },
        { "PAGE_CHAR",     "NOPAGE_CHAR",     TT_PAGE          },
        { "SECURE",        "NOSECURE",        TT_SECURE        },
        { "FALLBACK",      "NOFALLBACK",      TT_FALLBACK      },
        { "DIALUP",        "NODIALUP",        TT_DIALUP        },
        { "OPER",          "NOOPER",          TT_OPER          },
        { "ALTYPEAHD",     "NOALTYPEAHD",     TT_ALTYPEAHD     },
        { "RUNOUT",        "NORUNOUT",        TT_RUNOUT        },
    };

    for (unsigned i = 0; i < sizeof(quals)/sizeof(quals[0]); i++) {
        if (dcl_has_qualifier(cmd, quals[i].off)) {
            vms_terminal_set_char(term, quals[i].bit, 0);
            changed = 1;
        }
        if (dcl_has_qualifier(cmd, quals[i].on)) {
            vms_terminal_set_char(term, quals[i].bit, 1);
            changed = 1;
        }
    }

    /* Apply changes to real terminal */
    if (changed)
        vms_terminal_apply(term);

    return SS$_NORMAL;
}

/*
 * SET PROTECTION - Set file protection.
 * Format: SET PROTECTION (S:RWED,O:RW,G:R,W:) filespec
 */
static int cmd_set_protection(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 3) {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing protection string and/or file specification");
        return SS$_BADPARAM;
    }

    /* param[0] = "PROTECTION", param[1] = protection string, param[2] = filespec */
    const char *prot_str = cmd->params[1];
    const char *filespec = cmd->params[2];

    uint16_t vprot;
    if (vmsfs_parse_protection(prot_str, &vprot) != SS$_NORMAL) {
        dcl_error("DCL", 2, "BADPROT", "invalid protection string - %s", prot_str);
        return SS$_BADPARAM;
    }

    mode_t new_mode = vmsfs_protection_to_mode(vprot);

    char linux_path[1024];
    dcl_resolve_path(ctx, filespec, linux_path, sizeof(linux_path));

    if (chmod(linux_path, new_mode) != 0) {
        dcl_error("RMS", 2, "PRV", "failed to set protection - %s", vms_strerror(errno));
        return SS$_NOPRIV;
    }

    return SS$_NORMAL;
}

/*
 * SET PASSWORD - Change user password (interactive prompts).
 */
static int cmd_set_password(struct dcl_command *cmd)
{
    (void)cmd;
    struct dcl_context *ctx = dcl_get_context();

    printf("%%SET-I-PASSWORD, password change not fully implemented\n");
    printf("  User: %s\n", ctx->username);
    printf("  Full SYSUAF.DAT rewrite is planned for a future release.\n");

    return SS$_NORMAL;
}

/*
 * SET MESSAGE /FACILITY /NOFACILITY /SEVERITY /NOSEVERITY
 *             /IDENTIFICATION /NOIDENTIFICATION /TEXT /NOTEXT
 *
 * Controls which components of VMS error messages are displayed.
 * On OpenVMS, dcl_error() respects these flags when formatting output.
 */
static int cmd_set_message(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /[NO]FACILITY */
    if (dcl_has_qualifier(cmd, "NOFACILITY"))
        ctx->msg_facility = 0;
    else if (dcl_has_qualifier(cmd, "FACILITY"))
        ctx->msg_facility = 1;

    /* /[NO]SEVERITY */
    if (dcl_has_qualifier(cmd, "NOSEVERITY"))
        ctx->msg_severity = 0;
    else if (dcl_has_qualifier(cmd, "SEVERITY"))
        ctx->msg_severity = 1;

    /* /[NO]IDENTIFICATION */
    if (dcl_has_qualifier(cmd, "NOIDENTIFICATION") ||
        dcl_has_qualifier(cmd, "NOIDENT"))
        ctx->msg_ident = 0;
    else if (dcl_has_qualifier(cmd, "IDENTIFICATION") ||
             dcl_has_qualifier(cmd, "IDENT"))
        ctx->msg_ident = 1;

    /* /[NO]TEXT */
    if (dcl_has_qualifier(cmd, "NOTEXT"))
        ctx->msg_text = 0;
    else if (dcl_has_qualifier(cmd, "TEXT"))
        ctx->msg_text = 1;

    return SS$_NORMAL;
}

/*
 * SET CONTROL[=(item,...)] / SET NOCONTROL[=(item,...)]
 *
 * Enables or disables Ctrl-Y (and Ctrl-C) trapping.
 * SET CONTROL=Y   — enable Ctrl-Y interrupt
 * SET NOCONTROL=Y — disable Ctrl-Y interrupt
 *
 * The value is in cmd->params[1] for "SET CONTROL=Y" style,
 * or parsed from qualifiers. VMS also allows SET CONTROL=(Y,T).
 */
static int cmd_set_control(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /*
     * Determine enable vs disable from the subcommand word.
     * "CONTROL"   => enable
     * "NOCONTROL" => disable
     */
    int enable = 1;
    if (cmd->param_count >= 1 &&
        dcl_match_command(cmd->params[0], "NOCONTROL", 9))
        enable = 0;

    /*
     * The item list may be in params[1] (bare word after =) or in
     * the qualifier value when parsed as /CONTROL=Y.  Accept both.
     * Default target is Y when no item specified.
     */
    const char *item = (cmd->param_count >= 2) ? cmd->params[1] : "Y";

    /* Parse item list — may be "(Y)" or "Y" or "(Y,T)" */
    char item_buf[64];
    strncpy(item_buf, item, sizeof(item_buf) - 1);
    item_buf[sizeof(item_buf) - 1] = '\0';

    /* Strip surrounding parens */
    size_t ilen = strlen(item_buf);
    if (ilen > 0 && item_buf[0] == '(') {
        memmove(item_buf, item_buf + 1, ilen);
        ilen--;
    }
    if (ilen > 0 && item_buf[ilen - 1] == ')') {
        item_buf[ilen - 1] = '\0';
    }

    /* Walk comma-separated tokens */
    char *saveptr = NULL;
    char *tok = strtok_r(item_buf, ",", &saveptr);
    while (tok) {
        while (*tok == ' ') tok++;
        if (strcasecmp(tok, "Y") == 0) {
            ctx->ctrl_y_enabled = enable;
        }
        /* T = Ctrl-T (broadcast) — track but not implemented beyond flag */
        tok = strtok_r(NULL, ",", &saveptr);
    }

    return SS$_NORMAL;
}

/*
 * SET PROCESS /NAME=procname /PRIORITY=n /PRIVILEGES=(priv,...)
 *
 * /NAME=    — rename the process name (stored in context; no OS rename)
 * /PRIORITY= — set base priority; requires ALTPRI privilege
 * /PRIVILEGES= — set process privileges; requires SETPRV or OPER
 */
static int cmd_set_process(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /*
     * /NAME=name (vms-fbe) -- MATCH VMS: the name is executive-resident
     * state ($SETPRN writes the PCB directory, not process-local memory),
     * so this now calls vms_kif_setprn() and lets the executive decide
     * legality and uniqueness, the same VMS_IOCTL_SETPRN path $CREPRC
     * already uses (src/libvms/syssvc/sys_process.c). Before this change
     * the qualifier only wrote ctx->process_name, a per-DCL-process
     * struct: SET PROCESS/NAME=X then F$PROCESS() read the value back out
     * of the SAME struct it had just written, so no other process --
     * SHOW SYSTEM, $GETJPI by name or pid -- could ever see it (Rule 11:
     * the A-writes/B-reads test). vms_kif_procscan() (SHOW SYSTEM) and
     * vms_kif_getjpi_prcnam() ($GETJPI) already read prcnam out of the
     * executive's struct vms_proc row, so writing that row is sufficient
     * to make this real -- no new reader needed.
     *
     * ctx->process_name is still updated on success, because
     * dcl_lexical.c's F$PROCESS() and the prompt formatter read it (that
     * file is out of scope this round -- see the item's scope-discipline
     * note) and it should not go stale relative to what was actually set.
     * On a REFUSAL (SS$_DUPLNAM / SS$_IVLOGNAM) it is deliberately left
     * untouched, so a rejected rename does not locally claim a name the
     * executive refused to record -- that would recreate the same
     * facade for the failure path this fix closes for the success path.
     */
    const char *name_val = dcl_qualifier_value(cmd, "NAME");
    if (name_val && *name_val) {
        char upname[sizeof(ctx->process_name)];
        strncpy(upname, name_val, sizeof(upname) - 1);
        upname[sizeof(upname) - 1] = '\0';
        /* Upper-case the name, VMS style, before it goes on the wire --
         * the executive validates and stores exactly what it is given. */
        for (size_t i = 0; upname[i]; i++)
            upname[i] = (char)toupper((unsigned char)upname[i]);

        uint32_t st = vms_kif_setprn(upname);
        if (!(st & 1)) {
            /* SS$_DUPLNAM (148) and SS$_IVLOGNAM (340) are ORACLE-PINNED
             * (vms-8019, see ssdef.h) against real OpenVMS VAX V7.3:
             * F$MESSAGE(148)  -> %SYSTEM-F-DUPLNAM,  duplicate name
             * F$MESSAGE(340)  -> %SYSTEM-F-IVLOGNAM, invalid logical name
             * Both are severity F (index 4 in vms_severity_char's table)
             * regardless of the pinning comment's stale section heading.
             * These are the ONLY two failure statuses
             * src/kernel/vms_proctab.c's vms_ioctl_setprn() sets in
             * args.status -- read there before assuming a third belongs
             * here (Method 5: this is a checked "only", not an assumed
             * one). A status OTHER than these two means vms_kif_setprn's
             * KIF_CALL never reached the kernel handler at all (the
             * device could not be opened or the ioctl itself failed) --
             * a condition Rule 9 makes unreachable in the product (PID 1
             * refuses to boot without the executive, and this DCL
             * session already proved /dev/vms open at startup via
             * vms_kif_getjpi_self). It is reported honestly, with NO
             * invented VMS message text and NO "%SYSTEM-" facility
             * (Rule 10: that would self-certify a fake VMS message for a
             * condition VMS never shows), and the raw status is printed
             * as the only fact known. */
            if (st == SS$_DUPLNAM)
                dcl_error("SYSTEM", 4, "DUPLNAM", "duplicate name");
            else if (st == SS$_IVLOGNAM)
                dcl_error("SYSTEM", 4, "IVLOGNAM", "invalid logical name");
            else
                dcl_error("OVMX", 2, "SETPRNFAIL",
                          "SET PROCESS/NAME could not reach the executive "
                          "(status %%X%08X)", st);
            return (int)st;
        }

        strncpy(ctx->process_name, upname, sizeof(ctx->process_name) - 1);
        ctx->process_name[sizeof(ctx->process_name) - 1] = '\0';
    }

    /*
     * /PRIORITY=n — requires ALTPRI. Gated on enforced_privs_held(), NOT
     * ctx->privileges (vms-2b8 round 5: see that function's comment for
     * the measured desync this replaces -- ALTPRI is not in
     * VMS_PRV_M_ENFORCED, so this now refuses for every identity until
     * vms-pv1 gives ALTPRI real enforcement to match).
     */
    const char *pri_val = dcl_qualifier_value(cmd, "PRIORITY");
    if (pri_val && *pri_val) {
        uint64_t held = enforced_privs_held();
        if (!(held & PRV$M_ALTPRI) &&
            !(held & PRV$M_SYSPRV) &&
            !(held & PRV$M_BYPASS)) {
            /* Same HIDE wording as SET TIME's gate below, for the same
             * reason (vms-2b8 round 7): this is true for every caller
             * regardless of what its SYSUAF record authorizes, because
             * ALTPRI is not in VMS_PRV_M_ENFORCED -- nothing this build
             * enforces, not something a particular account lacks. */
            dcl_error("SET", 2, "NOPRIV",
                      "no privilege for SET PROCESS /PRIORITY -- this "
                      "privilege is not enforced on this system (vms-pv1); "
                      "no identity can pass this check until that lands");
            return SS$_NOPRIV;
        }
        char *endp;
        int pri = (int)strtol(pri_val, &endp, 10);
        if (endp == pri_val || *endp != '\0' || pri < 0 || pri > 31) {
            dcl_error("SET", 2, "INVPRI",
                      "invalid priority \\%d\\ - must be 0-31", pri);
            return SS$_BADPARAM;
        }
        ctx->process_priority = pri;
        /* Best-effort: try to set Linux scheduling niceness proportionally */
        /* VMS pri 0 = lowest, 15 = normal, 31 = highest
         * Linux nice: -20 (highest) to +19 (lowest) */
        int nice_val = 19 - (pri * 39) / 31;
        setpriority(PRIO_PROCESS, 0, nice_val);
    }

    /*
     * /PRIVILEGES=(priv,...) — requires SETPRV or OPER. Gated on
     * enforced_privs_held() (vms-2b8 round 5), the same source
     * F$PRIVILEGE and SHOW PROCESS/PRIVILEGES read; SETPRV IS in
     * VMS_PRV_M_ENFORCED, so a genuinely SETPRV-holding identity still
     * passes this gate.
     */
    const char *privs_val = dcl_qualifier_value(cmd, "PRIVILEGES");
    if (privs_val && *privs_val) {
        uint64_t held = enforced_privs_held();
        if (!(held & PRV$M_SETPRV) &&
            !(held & PRV$M_SYSPRV) &&
            !(held & PRV$M_BYPASS)) {
            dcl_error("SET", 2, "NOPRIV",
                      "no privilege for SET PROCESS /PRIVILEGES");
            return SS$_NOPRIV;
        }
        /*
         * DOES NOT REACH THE EXECUTIVE, AND SAYS SO (vms-2b8 round 4,
         * Rule 10's HIDE answer). This used to overwrite ctx->privileges
         * outright with parse_privilege_string(pv) -- a local,
         * unauthenticated self-assertion with no connection to the
         * executive's real mask. MEASURED, this was a live bug: it
         * silently discarded whatever ctx->privileges held (including
         * SETPRV/CMKRNL/CMEXEC/WORLD, the executive's real enforced set)
         * and replaced it with only the newly-named privilege, so the
         * VERY NEXT SET PROCESS/PRIVILEGES call on the same session could
         * fail its own SETPRV gate above -- a session locking itself out
         * of a command it was genuinely authorized for, purely because an
         * unrelated prior call had clobbered the local cache. And because
         * dcl_lexical.c's F$PRIVILEGE used to read that same
         * ctx->privileges, this command could make F$PRIVILEGE and SHOW
         * PROCESS/PRIVILEGES (which reads the executive directly) disagree
         * about the SAME process at the SAME moment -- this item's whole
         * subject. F$PRIVILEGE no longer reads ctx->privileges at all (it
         * asks the executive fresh, every call), so that specific
         * contradiction is now structurally impossible regardless of what
         * this command does. But the command still cannot honestly claim
         * to have changed anything the executive enforces: MATCH VMS would
         * mean calling vms_kif_setprv() (src/libvmssys/vms_kif.c), which
         * exists but is declared OVMX-UNWIRED in vms_kif.h pending vms-pv1
         * ($SETPRV wiring is that item's job, not this one's, and this
         * round is under an explicit instruction not to touch that
         * declaration or the vms_kif caller census while it is mid-flight
         * on another branch). So this is HIDE, not MATCH, until vms-pv1
         * lands: no local mutation, and a loud, honest diagnostic instead
         * of silent success.
         *
         * SEVERITY LETTER FIXED TO I, round 5: this printed as a WARNING
         * (%OVMX-W-) while the function falls through to `return
         * SS$_NORMAL` below -- a success status. On VMS the severity
         * field's low bit is the success/failure discriminator DCL's own
         * $STATUS and ON-condition logic branch on: W and F are the
         * "unsuccessful" side (bit clear), S and I are "successful" (bit
         * set), so a W-severity message paired with a SS$_NORMAL return
         * is not cosmetic -- it is an OVMX-invented message disagreeing
         * with the OVMX-invented status it sits next to. This is an
         * OVMX-facility diagnostic for a condition VMS does not have
         * (Rule 10 allows that; it is not the VMS SETPRV facility's own
         * voice), so there is no VMS wording to match -- only its own
         * severity to make consistent with its own status, matching the
         * I + SS$_NORMAL pairing this file already uses for other
         * acknowledged-but-inert commands (cmd_set_host's
         * %SET-I-NOTAVAIL, cmd_set_audit's %SET-I-INTSET).
         */
        printf("%%OVMX-I-NOSETPRV, SET PROCESS/PRIVILEGES does not change "
               "the executive's privilege state on this system yet "
               "(vms-pv1) -- no privileges were changed\n");
    }

    return SS$_NORMAL;
}

/*
 * SET FILE /VERSION_LIMIT=n /EXPIRATION_DATE=ddmmyyyy
 *
 * /VERSION_LIMIT — sets ODS-2 version limit on a file.
 *   Under Linux this is advisory (no direct FS support); we record
 *   the intent but cannot enforce it at the kernel level.
 * /EXPIRATION_DATE — sets the file expiration date (utimes).
 */
static int cmd_set_file(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* Need at least a filespec after SET FILE */
    if (cmd->param_count < 2) {
        dcl_error("SET", 2, "NOFILES",
                  "missing file specification");
        return SS$_BADPARAM;
    }

    const char *filespec = cmd->params[1];
    char linux_path[1024];
    dcl_resolve_path(ctx, filespec, linux_path, sizeof(linux_path));

    struct stat st;
    if (stat(linux_path, &st) != 0) {
        dcl_error("SET", 2, "NOSUCHFILE",
                  "file not found - \\%s\\", filespec);
        return SS$_NOSUCHFILE;
    }

    /* /VERSION_LIMIT=n — advisory; just validate and acknowledge */
    const char *vl = dcl_qualifier_value(cmd, "VERSION_LIMIT");
    if (vl && *vl) {
        char *endp;
        int vlim = (int)strtol(vl, &endp, 10);
        if (endp == vl || *endp != '\0' || vlim < 0 || vlim > 32767) {
            dcl_error("SET", 2, "INVVLIM",
                      "invalid version limit \\%s\\", vl);
            return SS$_BADPARAM;
        }
        /* On Linux/ext4 there is no native version-limit support.
         * We acknowledge the setting without error. */
    }

    /* /EXPIRATION_DATE=dd-mmm-yyyy or absolute quadword in decimal.
     * Accept VMS date string format: dd-MMM-yyyy[:hh:mm:ss] */
    const char *exp_date = dcl_qualifier_value(cmd, "EXPIRATION_DATE");
    if (exp_date && *exp_date) {
        struct tm exp_tm;
        memset(&exp_tm, 0, sizeof(exp_tm));

        /* Parse VMS date: dd-MMM-yyyy or dd-MMM-yyyy:hh:mm:ss */
        static const char *mon_names[] = {
            "JAN","FEB","MAR","APR","MAY","JUN",
            "JUL","AUG","SEP","OCT","NOV","DEC"
        };

        char date_buf[64];
        strncpy(date_buf, exp_date, sizeof(date_buf) - 1);
        date_buf[sizeof(date_buf) - 1] = '\0';
        /* Upper-case for comparison */
        for (size_t i = 0; date_buf[i]; i++)
            date_buf[i] = (char)toupper((unsigned char)date_buf[i]);

        int day = 0, mon = -1, year = 0;
        int hr = 0, mi = 0, sc = 0;
        char mon_str[4] = {0};

        /* Try dd-MMM-yyyy:hh:mm:ss then dd-MMM-yyyy */
        int parsed = 0;
        if (sscanf(date_buf, "%d-%3s-%d:%d:%d:%d",
                   &day, mon_str, &year, &hr, &mi, &sc) >= 3)
            parsed = 1;
        else if (sscanf(date_buf, "%d-%3s-%d", &day, mon_str, &year) == 3)
            parsed = 1;

        if (parsed) {
            for (int m = 0; m < 12; m++) {
                if (strncmp(mon_str, mon_names[m], 3) == 0) {
                    mon = m;
                    break;
                }
            }
        }

        if (!parsed || mon < 0) {
            dcl_error("SET", 2, "IVTIME",
                      "invalid expiration date - \\%s\\", exp_date);
            return SS$_IVTIME;
        }

        exp_tm.tm_mday = day;
        exp_tm.tm_mon  = mon;
        exp_tm.tm_year = year - 1900;
        exp_tm.tm_hour = hr;
        exp_tm.tm_min  = mi;
        exp_tm.tm_sec  = sc;
        exp_tm.tm_isdst = -1;

        time_t exp_t = mktime(&exp_tm);
        if (exp_t == (time_t)-1) {
            dcl_error("SET", 2, "IVTIME",
                      "cannot convert expiration date - \\%s\\", exp_date);
            return SS$_IVTIME;
        }

        /* Set atime = now, mtime = expiration date */
        struct timeval tv[2];
        gettimeofday(&tv[0], NULL);
        tv[1].tv_sec  = exp_t;
        tv[1].tv_usec = 0;
        if (utimes(linux_path, tv) != 0) {
            dcl_error("SET", 2, "PRV",
                      "cannot set expiration date - %s", vms_strerror(errno));
            return SS$_NOPRIV;
        }
    }

    return SS$_NORMAL;
}

/*
 * SET UIC [uic]
 *
 * Changes the current process UIC (user identification code).
 * On OpenVMS: SET UIC [group,member]
 * Requires SETPRV, SYSPRV, or BYPASS privilege to change to another UIC.
 * Without privilege, only reports the current UIC.
 */
static int cmd_set_uic(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        /* No argument: display current UIC */
        printf("  Current UIC: [%03o,%03o]\n", ctx->uic_group, ctx->uic_member);
        return SS$_NORMAL;
    }

    const char *uic_str = cmd->params[1];

    /* Check privilege — enforced_privs_held(), not ctx->privileges
     * (vms-2b8 round 5; see that function's comment). SETPRV is
     * enforced, so this still passes for a SETPRV-holding identity. */
    {
        uint64_t held = enforced_privs_held();
        if (!(held & PRV$M_SETPRV) &&
            !(held & PRV$M_SYSPRV) &&
            !(held & PRV$M_BYPASS)) {
            dcl_error("SET", 2, "NOPRIV",
                      "no privilege for SET UIC");
            return SS$_NOPRIV;
        }
    }

    /* Parse [group,member] in octal — strip brackets */
    char uic_buf[64];
    strncpy(uic_buf, uic_str, sizeof(uic_buf) - 1);
    uic_buf[sizeof(uic_buf) - 1] = '\0';

    size_t ulen = strlen(uic_buf);
    if (ulen > 0 && uic_buf[0] == '[') {
        memmove(uic_buf, uic_buf + 1, ulen);
        ulen--;
    }
    if (ulen > 0 && uic_buf[ulen - 1] == ']') {
        uic_buf[ulen - 1] = '\0';
    }

    unsigned int grp = 0, mem = 0;
    if (sscanf(uic_buf, "%o,%o", &grp, &mem) != 2) {
        dcl_error("SET", 2, "IVUIC",
                  "invalid UIC format - \\%s\\ (expected [group,member])", uic_str);
        return SS$_BADPARAM;
    }

    ctx->uic_group  = grp;
    ctx->uic_member = mem;

    return SS$_NORMAL;
}

/*
 * SET WORKING_SET /QUOTA=n /EXTENT=n /LIMIT=n
 *
 * Controls process working set size.  On Linux we map /QUOTA to
 * RLIMIT_AS (virtual address space) as the closest approximation.
 * VMS page size is 512 bytes; values are in pages.
 */
static int cmd_set_working_set(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

#define VMS_PAGE_SIZE 512

    /* /QUOTA=n pages */
    const char *quota_val = dcl_qualifier_value(cmd, "QUOTA");
    if (quota_val && *quota_val) {
        char *endp;
        int pages = (int)strtol(quota_val, &endp, 10);
        if (endp == quota_val || *endp != '\0' || pages < 0) {
            dcl_error("SET", 2, "INVQUO",
                      "invalid working set quota \\%s\\", quota_val);
            return SS$_BADPARAM;
        }
        ctx->ws_quota = pages;

        /* Best-effort: adjust RLIMIT_DATA */
        if (pages > 0) {
            struct rlimit rl;
            if (getrlimit(RLIMIT_DATA, &rl) == 0) {
                rlim_t new_limit = (rlim_t)pages * VMS_PAGE_SIZE;
                if (rl.rlim_max == RLIM_INFINITY || new_limit <= rl.rlim_max) {
                    rl.rlim_cur = new_limit;
                    setrlimit(RLIMIT_DATA, &rl);
                }
            }
        }
    }

    /* /EXTENT=n and /LIMIT=n are also valid — acknowledge silently */
    /* (EXTENT = maximum working set, LIMIT = minimum guaranteed pages) */

    return SS$_NORMAL;
#undef VMS_PAGE_SIZE
}

/*
 * SET TIME [dd-mmm-yyyy:hh:mm:ss] — set system clock (privileged).
 *
 * Requires OPER or SYSPRV privilege.  Uses settimeofday(2).
 */
static int cmd_set_time(struct dcl_command *cmd)
{
    if (cmd->param_count < 2) {
        /* No argument: display current time (same as SHOW TIME).
         * Reading the clock requires no privilege on VMS or Linux. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        static const char *mon_abbr[] = {
            "JAN","FEB","MAR","APR","MAY","JUN",
            "JUL","AUG","SEP","OCT","NOV","DEC"
        };
        printf("  %2d-%s-%04d %02d:%02d:%02d.%02d\n",
               tm.tm_mday, mon_abbr[tm.tm_mon], 1900 + tm.tm_year,
               tm.tm_hour, tm.tm_min, tm.tm_sec,
               (int)(ts.tv_nsec / 10000000));
        return SS$_NORMAL;
    }

    /*
     * Privilege check — required when actually setting the clock.
     * enforced_privs_held(), not ctx->privileges (vms-2b8 round 5; see
     * that function's comment).
     *
     * REGRESSION, DISCLOSED (vms-2b8 round 6). OPER, SYSPRV and BYPASS
     * are ALL absent from VMS_PRV_M_ENFORCED, so this gate can no
     * longer be passed by ANY identity, not just an unprivileged one --
     * a real change in behaviour from before round 5, when this read
     * the raw, unmasked ctx->privileges and SYSTEM's SYSUAF record
     * (authorized for privilege ALL, OPER included) let it through.
     * MEASURED this round, real podman-built QEMU boot, SYSTEM session
     * (the maximal-privilege SYSUAF account): SET TIME is refused even
     * here, so no lesser-privileged identity can succeed either -- the
     * mask VMS_PRV_M_ENFORCED applies is fixed at compile time, not a
     * per-session grant a stronger identity could hold. This is Rule
     * 10's HIDE answer, applied honestly rather than left implicit: a
     * bare %SET-E-NOPRIV reads as "your account needs OPER, go get it",
     * which is false on this build -- no account can. Say so in the
     * message text instead of leaving the reader to infer it, exactly
     * as the round-5 fix already does for SET PROCESS/PRIVILEGES's
     * %OVMX-I-NOSETPRV. Restoring real grantability is vms-pv1's job
     * (wiring vms_kif_setprv into VMS_PRV_M_ENFORCED); this round only
     * has to stop hiding that the capability disappeared.
     *
     * ROUND 7: the message this printed named SYSUAF ("OPER is
     * authorized by SYSUAF but not yet enforced") -- true of SYSTEM and
     * OPERATOR, whose SYSUAF records do hold OPER, and FALSE of the
     * other four shipped accounts (GUEST, DEFAULT, USER1, USER2 hold no
     * OPER at all -- GUEST is TMPMBX only, DEFAULT/USER1/USER2 add
     * NETMBX, neither is OPER; see distro/rootfs/vms/SYS0/SYSCOMMON/
     * SYSEXE/SYSUAF.DAT), because this code path (enforced_privs_held()
     * above) reads the executive's live cur_privs and masks it with
     * the compile-time-fixed VMS_PRV_M_ENFORCED, which has no OPER bit
     * at all (src/kernel/vms_ioctl.h) -- the check's answer is the
     * same for every identity regardless of its SYSUAF record.
     * A per-caller claim shipped to the console is either true for
     * every caller that can see it or it does not appear (standing
     * prose ruling, CLAUDE.md project rule 10). The corrected text
     * below says only what is true regardless of who is asking: OVMX
     * does not enforce this privilege yet, for anyone.
     */
    uint64_t held = enforced_privs_held();
    if (!(held & PRV$M_OPER) &&
        !(held & PRV$M_SYSPRV) &&
        !(held & PRV$M_BYPASS)) {
        dcl_error("SET", 2, "NOPRIV",
                  "no privilege for SET TIME -- this privilege is not "
                  "enforced on this system (vms-pv1); no identity can "
                  "pass this check until that lands");
        return SS$_NOPRIV;
    }

    const char *time_str = cmd->params[1];

    /* Parse VMS format: dd-MMM-yyyy:hh:mm:ss or hh:mm:ss (time only) */
    static const char *mon_names[] = {
        "JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"
    };

    char ts_buf[64];
    strncpy(ts_buf, time_str, sizeof(ts_buf) - 1);
    ts_buf[sizeof(ts_buf) - 1] = '\0';
    for (size_t i = 0; ts_buf[i]; i++)
        ts_buf[i] = (char)toupper((unsigned char)ts_buf[i]);

    struct tm new_tm;
    memset(&new_tm, 0, sizeof(new_tm));

    /* Get current local time as base */
    time_t now = time(NULL);
    localtime_r(&now, &new_tm);

    int day = 0, mon = -1, year = 0;
    int hr = 0, mi = 0, sc = 0;
    char mon_str[4] = {0};
    int have_date = 0;

    /* Try full datetime first, then time-only */
    if (sscanf(ts_buf, "%d-%3s-%d:%d:%d:%d",
               &day, mon_str, &year, &hr, &mi, &sc) >= 3) {
        have_date = 1;
    } else if (sscanf(ts_buf, "%d:%d:%d", &hr, &mi, &sc) >= 2) {
        /* time only — keep current date */
    } else {
        dcl_error("SET", 2, "IVTIME",
                  "invalid time specification - \\%s\\", time_str);
        return SS$_IVTIME;
    }

    if (have_date) {
        for (int m = 0; m < 12; m++) {
            if (strncmp(mon_str, mon_names[m], 3) == 0) {
                mon = m;
                break;
            }
        }
        if (mon < 0) {
            dcl_error("SET", 2, "IVTIME",
                      "invalid month - \\%s\\", mon_str);
            return SS$_IVTIME;
        }
        new_tm.tm_mday = day;
        new_tm.tm_mon  = mon;
        new_tm.tm_year = year - 1900;
    }

    new_tm.tm_hour   = hr;
    new_tm.tm_min    = mi;
    new_tm.tm_sec    = sc;
    new_tm.tm_isdst  = -1;

    time_t new_t = mktime(&new_tm);
    if (new_t == (time_t)-1) {
        dcl_error("SET", 2, "IVTIME",
                  "cannot convert time - \\%s\\", time_str);
        return SS$_IVTIME;
    }

    struct timeval tv;
    tv.tv_sec  = new_t;
    tv.tv_usec = 0;

    if (settimeofday(&tv, NULL) != 0) {
        if (errno == EPERM) {
            dcl_error("SET", 2, "NOPRIV",
                      "cannot set system time - insufficient OS privilege");
            return SS$_NOPRIV;
        }
        dcl_error("SET", 2, "IVTIME",
                  "cannot set system time - %s", vms_strerror(errno));
        return SS$_IVTIME;
    }

    return SS$_NORMAL;
}

/*
 * SET HOST - Attempt DECnet connection (not available).
 */
static int cmd_set_host(struct dcl_command *cmd)
{
    (void)cmd;
    printf("%%SET-I-NOTAVAIL, DECnet is not available on this system\n");
    return SS$_NORMAL;
}

/*
 * SET AUDIT /ENABLE /DISABLE - Toggle security auditing.
 */
static int cmd_set_audit(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (dcl_has_qualifier(cmd, "ENABLE")) {
        ctx->audit_enabled = 1;
        printf("%%SET-I-INTSET, auditing enabled\n");
    } else if (dcl_has_qualifier(cmd, "DISABLE")) {
        ctx->audit_enabled = 0;
        printf("%%SET-I-INTSET, auditing disabled\n");
    } else {
        printf("%%SET-I-INTSET, security auditing is %s\n",
               ctx->audit_enabled ? "enabled" : "disabled");
    }
    return SS$_NORMAL;
}

/*
 * SET ACCOUNTING /ENABLE /DISABLE - Toggle accounting.
 */
static int cmd_set_accounting(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (dcl_has_qualifier(cmd, "ENABLE")) {
        ctx->accounting_enabled = 1;
        printf("%%SET-I-INTSET, accounting enabled\n");
    } else if (dcl_has_qualifier(cmd, "DISABLE")) {
        ctx->accounting_enabled = 0;
        printf("%%SET-I-INTSET, accounting disabled\n");
    } else {
        printf("%%SET-I-INTSET, accounting is %s\n",
               ctx->accounting_enabled ? "enabled" : "disabled");
    }
    return SS$_NORMAL;
}

/*
 * SET VOLUME - Set volume characteristics (requires mounted VMSFS).
 */
static int cmd_set_volume(struct dcl_command *cmd)
{
    (void)cmd;
    printf("%%SET-I-NOTIMPL, SET VOLUME requires a mounted VMSFS volume\n");
    return SS$_NORMAL;
}

/*
 * SET Dispatcher
 */
int cmd_set(struct dcl_command *cmd)
{
    if (cmd->param_count < 1) {
        dcl_error("DCL", 2, "NOKEYW", "missing keyword - supply what you want to set");
        return SS$_BADPARAM;
    }

    const char *subcmd = cmd->params[0];

    if (dcl_match_command(subcmd, "DEFAULT", 3))
        return cmd_set_default(cmd);
    if (dcl_match_command(subcmd, "PROMPT", 3))
        return cmd_set_prompt(cmd);
    if (dcl_match_command(subcmd, "VERIFY", 3) ||
        dcl_match_command(subcmd, "NOVERIFY", 3))
        return cmd_set_verify(cmd);
    if (dcl_match_command(subcmd, "TERMINAL", 4))
        return cmd_set_terminal(cmd);
    if (dcl_match_command(subcmd, "PROTECTION", 3))
        return cmd_set_protection(cmd);
    if (dcl_match_command(subcmd, "PASSWORD", 3))
        return cmd_set_password(cmd);
    if (dcl_match_command(subcmd, "NOON", 4)) {
        /* SET NOON — suppress ON ERROR handler at current level */
        struct dcl_context *noon_ctx = dcl_get_context();
        noon_ctx->noon_active = 1;
        return SS$_NORMAL;
    }
    if (dcl_match_command(subcmd, "ON", 2)) {
        /* SET ON — re-enable ON ERROR handler */
        struct dcl_context *on_ctx = dcl_get_context();
        on_ctx->noon_active = 0;
        return SS$_NORMAL;
    }
    if (dcl_match_command(subcmd, "MESSAGE", 3))
        return cmd_set_message(cmd);
    if (dcl_match_command(subcmd, "CONTROL", 4) ||
        dcl_match_command(subcmd, "NOCONTROL", 9))
        return cmd_set_control(cmd);
    if (dcl_match_command(subcmd, "PROCESS", 3))
        return cmd_set_process(cmd);
    if (dcl_match_command(subcmd, "FILE", 4))
        return cmd_set_file(cmd);
    if (dcl_match_command(subcmd, "UIC", 3))
        return cmd_set_uic(cmd);
    if (dcl_match_command(subcmd, "WORKING_SET", 4))
        return cmd_set_working_set(cmd);
    if (dcl_match_command(subcmd, "TIME", 4))
        return cmd_set_time(cmd);
    if (dcl_match_command(subcmd, "HOST", 3))
        return cmd_set_host(cmd);
    if (dcl_match_command(subcmd, "AUDIT", 3))
        return cmd_set_audit(cmd);
    if (dcl_match_command(subcmd, "ACCOUNTING", 3))
        return cmd_set_accounting(cmd);
    if (dcl_match_command(subcmd, "VOLUME", 3))
        return cmd_set_volume(cmd);
    if (dcl_match_command(subcmd, "ENTRY", 3))
        return cmd_set_entry(cmd);
    if (dcl_match_command(subcmd, "QUEUE", 3))
        return cmd_set_queue(cmd);

    dcl_error("DCL", 2, "IVKEYW", "unrecognized SET keyword - \\%s\\", subcmd);
    return SS$_IVKEYW;
}
