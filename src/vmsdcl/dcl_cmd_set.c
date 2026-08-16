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
#include "dcl/cdu.h"
#include "dcl/dcl_cmd.h"
#include "dcl/vms_messages.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vms/privs.h"
#include "prv_names.h"    /* VMS_PRIV_NAME_LIST -- the VMS privilege keyword
                           * dictionary, the SAME single source SHOW
                           * PROCESS/PRIVILEGES renders from (dcl_cmd_show.c) */
#include "starlet.h"
#include "vmsfs/filespec.h"
#include "vms/pcb.h"
#include "vms_kif.h"
#include "sysuaf.h"
#include "uaidef.h"    /* UAI$M_LOCKPWD (vms-c8fa) */
#include "sha256.h"
#include "ovmx_accounting.h"

#if defined(__linux__)
/* ODS-2 runtime flip (epic vms-5eb, rung R3): SET DEFAULT to a SYS$DISK
 * directory validates the directory through the genuine-ODS-2 volume handle
 * (the real MFD / FID chains), NOT a POSIX stat of the /vms passthrough --
 * closing the vms-272 defect class. Linux-only; the netbsd-vax cross keeps its
 * POSIX validation. See docs/design-ods2-runtime-flip.md. */
#include "vmsfs/sysdisk.h"
#include "ssdef.h"
#include "stsdef.h"
#endif

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

#if defined(__linux__)
/* Directory-existence probe callback for the SYS$DISK ODS-2 validation path:
 * existence is proven by the directory resolve, so this consumes records and
 * asks nothing of them. */
static int set_default_noop_cb(const char *name, unsigned name_len,
                               uint16_t version, const ods2_fid_t *fid,
                               void *ctx)
{
    (void)name; (void)name_len; (void)version; (void)fid; (void)ctx;
    return 0;
}
#endif

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

#if defined(__linux__)
    /* A SYS$DISK directory is validated against the genuine ODS-2 volume (the
     * real MFD), not a POSIX stat of the /vms passthrough (rung R3). A
     * device-not-mounted result is surfaced HONESTLY (Rule 9 / INV-6). */
    if (ods2_sysdisk_owns_path(check_path)) {
        int lst = ods2_sysdisk_list_dir(check_path, set_default_noop_cb, NULL);
        if (lst == SS$_DEVNOTMOUNT) {
            dcl_error("SYSTEM", STS$K_ERROR, "DEVNOTMOUNT",
                      "device not mounted - \\%s\\", dirspec);
            return SS$_DEVNOTMOUNT;
        }
        if (lst != SS$_NORMAL) {
            dcl_error("DCL", 2, "DIRECT", "invalid directory - \\%s\\", dirspec);
            return SS$_NOSUCHFILE;
        }
    } else
#endif
    {
        struct stat st;
        if (stat(check_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            dcl_error("DCL", 2, "DIRECT", "invalid directory - \\%s\\", dirspec);
            return SS$_NOSUCHFILE;
        }
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

    /* Keep the executive default directory (pcb->default_dir) in step with the
     * DCL default. It is the store $GETDDIR/SYS$DISK resolution reads and the
     * one a spawned subprocess inherits (sys_process.c copies the parent PCB's
     * default_dir); leaving it at the init value made SET DEFAULT invisible to
     * the executive and to child processes (vms-272). ctx->default_dir is the
     * fully-merged "DEV:[DIR]" spec, so store it verbatim. Fail-honest: if no
     * PCB is reachable this is a no-op, matching the rest of the executive
     * surface. */
    vms_pcb_set_default_dir(ctx->default_dir);

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

    /*
     * vms-6f4 Phase 0 (docs/design-dcl-fidelity.md sec 5): SET TERMINAL is
     * the canary verb for the qualifier-grammar hole. struct dcl_verb
     * (dcl/cdu.h) carries no per-verb declaration of legal qualifiers for
     * ANY command, so parse_qualifier() (dcl_parser.c) upcases and stores
     * whatever token followed '/' with no reference to what SET TERMINAL
     * actually accepts -- SET TERMINAL/FDAFS silently returned
     * SS$_NORMAL. %DCL-W-IVQUAL was structurally unreachable.
     *
     * This is a TARGETED validation for this one verb, against the exact
     * qualifier set already implemented below (function header comment,
     * lines 195-206) -- it is NOT the general per-verb qualifier table
     * (that is Phase 1, vms-097). See run_resolve_qualifier() in
     * dcl_cmd_process.c for the existing precedent of a single verb
     * building its own table because the shared parser/dispatch offers no
     * way to reject a qualifier at all.
     *
     * Runs before any state is touched, so an invalid qualifier rejects
     * the whole line with nothing applied -- matching the oracle, where
     * DCL validates the qualifier table before the verb routine is ever
     * entered (VAX1 capture, dcl_cmd_process.c:726: "%DCL-W-IVQUAL,
     * unrecognized qualifier - check validity, spelling, and placement").
     */
    static const char *const terminal_known_qualifiers[] = {
        "WIDTH", "PAGE", "SPEED", "PARITY", "DEVICE_TYPE", "DEVICE",
        "ECHO", "WRAP", "BROADCAST", "TYPEAHEAD", "HOSTSYNC", "TTSYNC",
        "LINE_EDITING", "INSERT", "OVERSTRIKE", "SCOPE", "LOWERCASE",
        "UPPERCASE", "TAB", "MECHTAB", "HOLDSCREEN", "EIGHTBIT",
        "READSYNC", "PASTHRU", "ESCAPE", "FORM", "FULLDUP", "HALFDUP",
        "MODEM", "PAGE_CHAR", "SECURE", "FALLBACK", "DIALUP", "OPER",
        "ALTYPEAHD", "RUNOUT",
    };
    for (int qi = 0; qi < cmd->qualifier_count; qi++) {
        const char *qname = cmd->qualifiers[qi].name;
        int known = 0;
        for (size_t k = 0; k < sizeof(terminal_known_qualifiers) /
                                sizeof(terminal_known_qualifiers[0]); k++) {
            if (strcasecmp(qname, terminal_known_qualifiers[k]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            dcl_error("DCL", 0, "IVQUAL",
                      "unrecognized qualifier - check validity, spelling, "
                      "and placement - \\%s\\", qname);
            return SS$_IVQUAL;
        }
    }

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
 *
 * Syntax (VSI OpenVMS DCL Dictionary, SET PROTECTION):
 *   SET PROTECTION[=(ownership[:access][,...])] filespec[,...]
 * e.g.  SET PROTECTION=(S:RWED,O:RWED,G:RE,W:) FOO.TXT
 *       SET PROTECTION=W:RE FOO.TXT           (a single category)
 *       SET PROTECTION (S:RWED,O:RW) FOO.TXT  (positional, "=" omitted)
 *
 * The parenthesised protection list contains commas, which the generic DCL
 * token loop splits into separate positional parameters (and, worse, leaves
 * the filespec sitting in whatever params[] slot fell after the last comma).
 * So we do NOT use cmd->params[] here; we reparse cmd->raw_tail -- the raw,
 * unsplit argument tail after the verb -- which preserves the list intact.
 *
 * VMS merge semantics: a category the user does NOT name keeps the file's
 * CURRENT protection (DCL Dictionary: unspecified categories are unchanged).
 * We seed the protection word from the file's present mode and let
 * vmsfs_parse_protection() override only the named categories.
 */
static int cmd_set_protection(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* cmd->raw_tail == everything after "SET", e.g.
     * "PROTECTION=(S:RWED,O:RWED,G:RE,W:) FOO.TXT". */
    const char *p = cmd->raw_tail;
    while (*p == ' ' || *p == '\t') p++;

    /* Skip the "PROTECTION" keyword (any legal abbreviation): it ends at the
     * first space, '=', or '(' -- whichever comes first. */
    while (*p && *p != ' ' && *p != '\t' && *p != '=' && *p != '(') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '=') {
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }

    /* Extract the protection spec: a parenthesised list, or a single
     * category token up to the next whitespace. */
    char prot_str[128];
    size_t n = 0;
    if (*p == '(') {
        while (*p && *p != ')' && n < sizeof(prot_str) - 2) prot_str[n++] = *p++;
        if (*p == ')') prot_str[n++] = *p++;      /* include the closing paren */
    } else {
        while (*p && *p != ' ' && *p != '\t' && n < sizeof(prot_str) - 1)
            prot_str[n++] = *p++;
    }
    prot_str[n] = '\0';

    /* The filespec follows. */
    while (*p == ' ' || *p == '\t') p++;
    char filespec[512];
    size_t fn = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && fn < sizeof(filespec) - 1)
        filespec[fn++] = *p++;
    filespec[fn] = '\0';

    if (prot_str[0] == '\0' || filespec[0] == '\0') {
        dcl_error("DCL", 2, "NOKEYW",
                  "missing protection string and/or file specification");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, filespec, linux_path, sizeof(linux_path));

    /* Seed the protection word from the file's CURRENT protection so that
     * categories the user does not name are preserved (VMS merge semantics).
     * A missing file is %RMS-E-FNF, exactly as VMS reports it. */
    struct stat st;
    if (stat(linux_path, &st) != 0) {
        dcl_error("RMS", 2, "PRV",
                  "failed to set protection - %s", vms_strerror(ENOENT));
        return SS$_NOPRIV;
    }
    uint16_t vprot = vmsfs_mode_to_protection(st.st_mode);

    if (vmsfs_parse_protection(prot_str, &vprot) != SS$_NORMAL) {
        dcl_error("DCL", 2, "BADPROT", "invalid protection string - %s", prot_str);
        return SS$_BADPARAM;
    }

    mode_t new_mode = vmsfs_protection_to_mode(vprot);

    if (chmod(linux_path, new_mode) != 0) {
        dcl_error("RMS", 2, "PRV", "failed to set protection - %s", vms_strerror(errno));
        return SS$_NOPRIV;
    }

    return SS$_NORMAL;
}

/*
 * dcl_read_noecho_line - read one line from the controlling terminal with
 * echo suppressed, for the SET PASSWORD prompts below.
 *
 * Same termios technique tools/vms_login.c's read_password() uses at the
 * LOGIN Password: prompt -- a small, UI-only duplicate (terminal echo
 * control, not a SYSUAF format) rather than a shared library function,
 * since nothing about it is part of the SYSUAF format INV-1 guards.
 * Returns 0 on success (buf holds the trimmed line), -1 on EOF/read error.
 */
static int dcl_read_noecho_line(char *buf, size_t bufsz)
{
    struct termios old_term, new_term;
    int have_term = 0;

    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_term) == 0) {
        new_term = old_term;
        new_term.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);
        have_term = 1;
    }

    int rc = 0;
    if (fgets(buf, (int)bufsz, stdin) == NULL)
        rc = -1;

    if (have_term) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
        putchar('\n');
        fflush(stdout);
    }

    if (rc == 0) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
    }
    return rc;
}

/*
 * SET PASSWORD [/GENERATE[=n]] [/SECONDARY] [/SYSTEM]
 *
 * Public OpenVMS DCL Dictionary "SET PASSWORD" entry (Rule 8 clean-room
 * citation): format is "SET PASSWORD" -- NO parameters -- with exactly
 * three qualifiers, /GENERATE, /SECONDARY, /SYSTEM:
 *   https://www.digiater.nl/openvms/doc/ia64-v8.3/opsys/vmsos83/9996/9996pro_205.html
 *   https://wiki.vmssoftware.com/SET_PASSWORD
 * There is NO /USER= qualifier and no parameter that names another
 * account -- a DCL user can only ever change their OWN password this way.
 * Changing someone ELSE's password is AUTHORIZE's job (tools/vms_authorize.c
 * cmd_modify(), already gated on SYSPRV via check_privilege()).
 *
 * The interactive sequence and its no-echo behaviour are pinned from an
 * indexed public transcript of the OpenVMS User's Manual "Changing Your
 * Password" walkthrough (search-cached copy, session 2026-08-11):
 *     Old password:
 *     New password:
 *     Verification:
 *   "While answering these prompts, your keystrokes will not be displayed."
 *   "If you fail to enter the same new password twice, the password is not
 *   changed" -- and a correct old-password check gates the whole exchange.
 * /GENERATE's default length range (6-8 chars, n to n+2 for /GENERATE=n) and
 * the classic PWDMINIMUM default of 6 characters -- "enforced only by the
 * DCL command SET PASSWORD" -- are the same Dictionary entry and the
 * OpenVMS Password wiki page. OVMX's sysuaf_record_t carries no per-account
 * PWDMINIMUM/PWDLIFETIME field (vms-846's compact SYSUAF), so the Dictionary
 * DEFAULT is what is enforced here, not a per-account SYSGEN value.
 *
 * INV-DCL (docs/design-dcl-fidelity.md sec 3): this used to print
 * "%SET-I-PASSWORD, password change not fully implemented" and return
 * SS$_NORMAL without touching SYSUAF -- a success-toned lie for a no-op.
 * It now verifies the old password against the real SYSUAF hash
 * (sysuaf_authenticate()) and, on match, writes a real new hash back through
 * the ONE shared writer (sysuaf_write_record() -> sysuaf_format_record(),
 * vms-9b7/INV-1) -- the same record format AUTHORIZE and $SETUAI use.
 *
 * /SECONDARY and /SYSTEM are real VMS features OVMX does not implement
 * (no secondary-password field in sysuaf_record_t; no per-node system-
 * password subsystem at all) -- both honestly refuse rather than fake
 * success. /SYSTEM still gates on SECURITY privilege FIRST, matching the
 * Dictionary ("/SYSTEM ... Requires SECURITY privilege"), so an
 * unprivileged caller gets the authentic privilege error rather than an
 * NOTIMPL that leaks whether the (nonexistent) feature would otherwise
 * have worked. /GENERATE is likewise an honest refusal: OVMX has no
 * password-strength generator to produce the five candidates VMS offers.
 */
static int cmd_set_password(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* SET's dispatcher (cmd_set() below) is not yet retrofit with a nested
     * per-subcommand qualifier table (docs/dcl-verb-fidelity-scoreboard.md,
     * Phase 1 "Deferred" note: SET/SHOW need a nested table design). Until
     * that lands, PASSWORD validates its own qualifiers with the SAME
     * Phase 1 validator (dcl_validate_qualifiers(), src/vmsdcl/dcl_parser.c)
     * against a local table, rather than re-deriving the check by hand the
     * way the pre-Phase-1 SET TERMINAL canary had to. */
    static const struct dcl_qual_def password_quals[] = {
        { "GENERATE",  CDU_VT_VALUE, 0, NULL, NULL },
        { "SECONDARY", CDU_VT_NONE,  0, NULL, NULL },
        { "SYSTEM",    CDU_VT_NONE,  0, NULL, NULL },
        { NULL, 0, 0, NULL, NULL },
    };
    struct dcl_verb password_verb_shim = { .quals = password_quals };
    uint32_t qstatus = dcl_validate_qualifiers(&password_verb_shim, cmd);
    if (qstatus != SS$_NORMAL)
        return (int)qstatus;

    /* SET PASSWORD takes no parameters. cmd->params[0] is "PASSWORD" (the
     * subcommand keyword) itself; anything past it is an extra parameter
     * real DCL rejects (MSG_DCL_MAXPARM, dcl_messages.c). */
    if (cmd->param_count > 1) {
        dcl_error("DCL", 2, "MAXPARM", "too many parameters");
        return SS$_BADPARAM;
    }

    if (dcl_has_qualifier(cmd, "SECONDARY")) {
        dcl_error("DCL", 0, "NOTIMPL",
                  "secondary passwords are not implemented in OVMX - "
                  "no state changed");
        return SS$_UNSUPPORTED;
    }

    if (dcl_has_qualifier(cmd, "SYSTEM")) {
        /* enforced_privs_held(), not ctx->privileges -- see that function's
         * comment at the top of this file (vms-2b8 round 5). */
        uint64_t held = enforced_privs_held();
        if (!(held & PRV$M_SECURITY)) {
            dcl_error("SET", 2, "NOPRIV", "no privilege for attempted operation");
            return SS$_NOPRIV;
        }
        dcl_error("DCL", 0, "NOTIMPL",
                  "OVMX has no per-node system-password subsystem - "
                  "no state changed");
        return SS$_UNSUPPORTED;
    }

    if (dcl_has_qualifier(cmd, "GENERATE")) {
        dcl_error("DCL", 0, "NOTIMPL",
                  "/GENERATE password generation is not implemented in "
                  "OVMX - no state changed");
        return SS$_UNSUPPORTED;
    }

    /* Self-service change only (see the citation above): look up the
     * CALLER's own SYSUAF record. No privilege is required to change your
     * own password. */
    sysuaf_record_t rec;
    if (sysuaf_lookup(ctx->username, &rec) != 0) {
        dcl_error("UAF", 2, "NOSUCHUSER", "no such user %s in SYSUAF",
                  ctx->username);
        return SS$_NOSUCHID;
    }

    /*
     * LOCKPWD (vms-c8fa). "To prevent the user from changing the password, use
     * the LOCKPWD flag" -- OpenVMS Guide to System Security, Primary Passwords:
     * with LOCKPWD set "the security administrator controls all changes made
     * to the password," so a self-service SET PASSWORD is refused; only a
     * privileged manager may change it through AUTHORIZE. This is honored
     * BEFORE any password is prompted -- the user is told plainly rather than
     * walked through prompts whose result would be discarded. The %SET-F-
     * PWDLOCKED text is the documented VMS message for this condition.
     */
    if (sysuaf_flags_to_mask(rec.flags) & UAI$M_LOCKPWD) {
        dcl_error("SET", 4, "PWDLOCKED",
                  "password is locked to prevent change");
        return SS$_NOPRIV;
    }

    char old_pw[128], new_pw[128], verify_pw[128];

    printf("Old password: ");
    fflush(stdout);
    if (dcl_read_noecho_line(old_pw, sizeof(old_pw)) != 0) {
        printf("\n");
        return SS$_ABORT;
    }

    /* "User authorization failure" is the SAME text, and the SAME
     * grounding (a verbatim public OpenVMS Alpha V6.2 console transcript),
     * that tools/vms_login.c already prints for a failed sysuaf_authenticate()
     * check -- reused here rather than invented, because this is the same
     * check against the same SYSUAF hash, not a different condition. */
    if (!sysuaf_authenticate(&rec, old_pw)) {
        printf("\nUser authorization failure\n");
        return SS$_INVLOGIN;
    }

    printf("New password: ");
    fflush(stdout);
    if (dcl_read_noecho_line(new_pw, sizeof(new_pw)) != 0) {
        printf("\n");
        return SS$_ABORT;
    }

    printf("Verification: ");
    fflush(stdout);
    if (dcl_read_noecho_line(verify_pw, sizeof(verify_pw)) != 0) {
        printf("\n");
        return SS$_ABORT;
    }

    if (strcmp(new_pw, verify_pw) != 0) {
        /* Dictionary: "If you fail to enter the same new password twice,
         * the password is not changed." No exact %SET-facility message
         * text for this case was found in the public mirrors checked
         * (Rule 8 -- do not invent one); DCL/INCOMPAT is the existing
         * generic-mismatch identifier already used elsewhere in this
         * codebase (SS$_INCOMPAT, src/libvms/include/ssdef.h) rather than
         * a fabricated SET-specific code. */
        dcl_error("DCL", 2, "INCOMPAT",
                  "new password and verification do not match - "
                  "password not changed");
        return SS$_INCOMPAT;
    }

    /* Reject empty (consistent with sysuaf_authenticate()'s own rule that
     * an unset/empty hash never authenticates -- vms-08f) and enforce the
     * Dictionary's classic PWDMINIMUM default of 6, the only floor OVMX can
     * honour without a per-account SYSGEN field. */
    size_t new_len = strlen(new_pw);
    if (new_len == 0) {
        dcl_error("DCL", 2, "NOPSWD", "password may not be blank - "
                  "password not changed");
        return SS$_BADPARAM;
    }
#define OVMX_PWDMINIMUM_DEFAULT 6
    if (new_len < OVMX_PWDMINIMUM_DEFAULT) {
        dcl_error("DCL", 2, "PWDTOOSHORT",
                  "password must be at least %d characters - "
                  "password not changed", OVMX_PWDMINIMUM_DEFAULT);
        return SS$_BADPARAM;
    }
#undef OVMX_PWDMINIMUM_DEFAULT

    /* Real hash, real rewrite -- the same SHA256 scheme sysuaf_authenticate()
     * checks against and the same shared writer AUTHORIZE/$SETUAI use
     * (sysuaf_write_record() -> sysuaf_format_record(), INV-1: one format,
     * one writer). */
    char new_hash[65];
    sha256_hex((const uint8_t *)new_pw, new_len, new_hash);
    strncpy(rec.password_hash, new_hash, sizeof(rec.password_hash) - 1);
    rec.password_hash[sizeof(rec.password_hash) - 1] = '\0';

    if (sysuaf_write_record(&rec) != 0) {
        dcl_error("UAF", 2, "WRITEFAIL",
                  "unable to update SYSUAF.DAT - password not changed");
        return SS$_FILACCERR;
    }

    printf("%%SET-S-PASSWORD, password for user %s changed\n", ctx->username);
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
        /* T = Ctrl-T reflexive status line. The DCL input layer's Ctrl-T
         * (0x14) handler (dcl_main.c) gates the reflexive one-liner on this
         * flag; SET NOCONTROL=T disables it. Default off (VMS convention). */
        else if (strcasecmp(tok, "T") == 0) {
            ctx->ctrl_t_enabled = enable;
        }
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
/*
 * parse_priv_setlist - split a SET PROCESS/PRIVILEGES value into the mask to
 * ENABLE and the mask to DISABLE.
 *
 * The DCL parser (dcl_parser.c) preserves the surrounding parentheses of a
 * list value, so "/PRIVILEGES=(SYSPRV,NOCMKRNL)" arrives here as the literal
 * string "(SYSPRV,NOCMKRNL)" and "/PRIVILEGES=ALL" as "ALL". VMS negates a
 * single privilege by prefixing its keyword with NO -- e.g.
 * SET PROCESS/PRIVILEGE=(NOSETPRV,NOSYSPRV), the exact form the oracle drove
 * in docs/oracle/vax73-privileges.md §3/§5.2. `default_disable` (set by an
 * explicit /DISABLE) flips the sense of an un-prefixed keyword, and a NO
 * prefix flips it again, so the two combine by XOR; with neither /DISABLE nor
 * a NO prefix a keyword ENABLES, which is the plain VMS default.
 *
 * The keyword set is the VMS privilege dictionary itself: prv_names.h's
 * VMS_PRIV_NAME_LIST -- the SAME single source SHOW PROCESS/PRIVILEGES renders
 * from (src/vmsdcl/dcl_cmd_show.c) -- plus the ALL pseudo-keyword. No VMS
 * privilege name begins with the letters "NO", so a leading "NO" is an
 * unambiguous negation prefix, never part of a keyword.
 *
 * An unrecognized keyword is copied to *bad and the function returns -1
 * without producing any mask, so the caller applies NO privilege change at
 * all (VMS refuses the whole command on a bad keyword).
 *
 * Returns 0 on success (masks filled), -1 on an unknown keyword.
 */
static int parse_priv_setlist(const char *val, int default_disable,
                              uint64_t *enable, uint64_t *disable,
                              char *bad, size_t badsz)
{
    static const struct { const char *name; uint64_t bit; } priv_kw[] = {
#define PK(n, m, d) { #n, (m) },
        VMS_PRIV_NAME_LIST(PK)
#undef PK
        { NULL, 0 }
    };

    *enable = 0;
    *disable = 0;
    if (bad && badsz) bad[0] = '\0';
    if (!val || !*val)
        return 0;

    char buf[512];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip one layer of the surrounding parentheses the parser preserves. */
    char *p = buf;
    size_t blen = strlen(p);
    if (blen >= 2 && p[0] == '(' && p[blen - 1] == ')') {
        p[blen - 1] = '\0';
        p++;
    }

    char *saveptr = NULL;
    for (char *tok = strtok_r(p, ",", &saveptr); tok;
         tok = strtok_r(NULL, ",", &saveptr)) {
        /* Trim surrounding whitespace. */
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t tl = strlen(tok);
        while (tl > 0 && (tok[tl - 1] == ' ' || tok[tl - 1] == '\t'))
            tok[--tl] = '\0';
        if (tl == 0)
            continue;

        int negate = 0;
        const char *name = tok;
        if (tl > 2 && (tok[0] == 'N' || tok[0] == 'n') &&
                      (tok[1] == 'O' || tok[1] == 'o')) {
            negate = 1;
            name = tok + 2;
        }

        uint64_t bit;
        if (strcasecmp(name, "ALL") == 0) {
            bit = PRV$M_ALL;
        } else {
            int found = 0;
            for (int i = 0; priv_kw[i].name; i++) {
                if (strcasecmp(name, priv_kw[i].name) == 0) {
                    bit = priv_kw[i].bit;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (bad && badsz) {
                    strncpy(bad, tok, badsz - 1);
                    bad[badsz - 1] = '\0';
                }
                return -1;
            }
        }

        if (default_disable ^ negate)
            *disable |= bit;
        else
            *enable |= bit;
    }

    return 0;
}

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
        /*
         * upname MUST be sized VMS_PRCNAM_XFER (64), not
         * sizeof(ctx->process_name) (16) -- see the VMS_PRCNAM_XFER
         * comment in src/kernel/vms_ioctl.h. A 16-byte local buffer
         * truncates an oversized name to 15 significant characters
         * BEFORE it ever reaches vms_kif_setprn(), handing the
         * executive an already-legal-looking name and never giving it
         * the chance to refuse it -- the exact defect that comment
         * exists to prevent (vms-fbe round 1: a 16-char name silently
         * "succeeded", truncated, with no message, where real VMS
         * refuses it outright). Sizing this at VMS_PRCNAM_XFER lets an
         * oversized name arrive intact so name_is_valid() in
         * vms_proctab.c can see it is not NUL-terminated within
         * VMS_PRCNAM_SIZE and return SS$_IVLOGNAM.
         */
        char upname[VMS_PRCNAM_XFER];
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
             * as the only fact known.
             *
             * The DUPLNAM/IVLOGNAM report below is ORACLE-PINNED to
             * src/kernel/vms_ioctl.h:653-658 (VAX1, OpenVMS VAX V7.3):
             * SET PROCESS/NAME wraps the underlying system-service
             * failure in the command's own generic
             * "%SET-E-NOTSET, error modifying process name" and reports
             * the specific status on a CONTINUATION line
             * ("-SYSTEM-F-<ident>, <text>") -- NOT the bare
             * "%SYSTEM-F-<ident>" round 1 pinned, which was this
             * round's own guess and not anything observed on VAX1; this
             * round corrects it to match the transcript already in this
             * repo. The unreachable-executive branch below stays OUT of
             * this wrapper -- it is not a status $SETPRN can ever
             * return, so it keeps its own non-VMS-branded single-line
             * report per Rule 10. */
            if (st == SS$_DUPLNAM) {
                dcl_error("SET", 2, "NOTSET", "error modifying process name");
                fprintf(stderr, "-SYSTEM-F-DUPLNAM, duplicate name\n");
            } else if (st == SS$_IVLOGNAM) {
                dcl_error("SET", 2, "NOTSET", "error modifying process name");
                fprintf(stderr,
                        "-SYSTEM-F-IVLOGNAM, invalid logical name\n");
            } else
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
     * /PRIVILEGES=(priv,...) — HIDE->MATCH (vms-e5d7). The mutation is the
     * EXECUTIVE'S: this parses the keyword list into an enable/disable mask
     * pair and calls sys$setprv, which routes through vms_kif_setprv ->
     * VMS_IOCTL_SETPRV -> vms_ioctl_setprv (src/kernel/vms_access.c). The
     * executive authorizes the request against THIS process's AUTHORIZED
     * (permanent) mask and OWNS the result -- it is not a DCL-local decision.
     *
     * NO DCL PRE-GATE, DELIBERATELY. The block this replaces gated on
     * enforced_privs_held() and refused the whole command with SS$_NOPRIV
     * unless the caller already held SETPRV/SYSPRV/BYPASS. That gate is
     * WRONG against the oracle and had to go for faithful behaviour:
     *   - docs/oracle/vax73-privileges.md §3: a process WITHOUT SETPRV
     *     successfully ran SET PROCESS/PRIVILEGE=SYSPRV and got SYSPRV back
     *     (%X10000001) -- enabling a privilege ALREADY in the authorized mask
     *     needs no SETPRV. The pre-gate refused exactly that.
     *   - docs/oracle/vax73-privileges.md §5.2: a process with NOALL still
     *     ran SET PROCESS/PRIVILEGE=(NOALL). Disabling needs no privilege
     *     either; the pre-gate refused that too.
     * On real VMS, ANY process may ISSUE SET PROCESS/PRIVILEGE; the executive
     * decides what it actually gets. Removing the pre-gate does NOT weaken
     * security: the executive is the sole authority and still refuses to
     * widen a process past its authorized mask (SS$_NOTALLPRIV, the
     * authorized subset only -- proven for the DCL surface in
     * tests/qemu/test_syssvc_setprv_dcl.c and at the service layer in
     * tests/qemu/test_syssvc_setprv.c). The old gate was a redundant, wrong
     * userspace pre-check that BLOCKED the authentic partial-grant path.
     *
     * ctx->privileges is intentionally not written: F$PRIVILEGE and SHOW
     * PROCESS/PRIVILEGES both ask the executive fresh (dcl_lexical.c,
     * dcl_cmd_show.c), and sys$setprv already mirrors the executive's
     * resulting mask into pcb->cur_privs for the two in-process readers.
     */
    const char *privs_val = dcl_qualifier_value(cmd, "PRIVILEGES");
    if (privs_val && *privs_val) {
        /*
         * VMS negates per-keyword with a NO prefix. /ENABLE and /DISABLE are
         * honoured as the default direction for un-prefixed keywords (default
         * = enable), a superset that reduces to plain VMS behaviour when
         * neither is given. A NO prefix flips the direction of its keyword
         * regardless.
         */
        int default_disable = (dcl_qualifier_value(cmd, "DISABLE") != NULL);
        if (dcl_qualifier_value(cmd, "ENABLE") != NULL)
            default_disable = 0;

        uint64_t enable_mask = 0, disable_mask = 0;
        char bad[64];
        if (parse_priv_setlist(privs_val, default_disable,
                               &enable_mask, &disable_mask,
                               bad, sizeof(bad)) != 0) {
            /*
             * Unknown privilege keyword. %CLI-W-IVKEYW is the standard VMS
             * CLI keyword-validation message (VSI DCL Dictionary); the exact
             * terminal shape for this specific command was not oracle-captured
             * this session, so the text is the documented CLI wording, not a
             * self-certified SETPRV-facility invention. No privilege was
             * changed -- the whole list is rejected.
             */
            dcl_error("CLI", 0 /* W */, "IVKEYW",
                      "unrecognized keyword - check validity and spelling, "
                      "\\%s\\", bad);
            return SS$_BADPARAM;
        }

        uint64_t prev = 0;
        uint32_t st = SS$_NORMAL;

        /* Disabling is always allowed (oracle §3); apply it first so a mixed
         * list that also enables cannot leave a privilege the caller asked to
         * drop still set. */
        if (disable_mask) {
            uint32_t d = sys$setprv(0 /* disable */, &disable_mask, 0, &prev);
            if (!(d & 1))
                st = d;
        }
        if (enable_mask && (st & 1))
            st = sys$setprv(1 /* enable */, &enable_mask, 0, &prev);

        /*
         * Map the EXECUTIVE'S status to DCL output. The message TEXT, IDENT,
         * SEVERITY and FACILITY are oracle-pinned in
         * docs/oracle/vax73-privileges.md §1 (F$MESSAGE round-trips on VAX1,
         * OpenVMS VAX V7.3) and the STATUS the executive returns for each
         * condition is pinned in §3; see §8 for the one residual (the choice
         * to surface the message as a bare pass-through rather than a %SET-
         * wrapper was not captured on the oracle and is flagged there).
         */
        if (st & 1) {
            /* Success: VMS prints nothing (oracle §3: a successful
             * SET PROCESS/PRIVILEGE returns %X10000001 and is silent). */
        } else if (st == SS$_NOTALLPRIV) {
            dcl_error("SYSTEM", 0 /* W */, "NOTALLPRIV",
                      "not all requested privileges authorized");
        } else if (st == SS$_NOPRIV) {
            dcl_error("SYSTEM", 4 /* F */, "NOPRIV",
                      "insufficient privilege or object protection violation");
        } else {
            /*
             * The executive was unreachable (INV-6 / CLAUDE.md Rule 9). This
             * is a condition OpenVMS never faces, so it gets an OVMX-branded
             * honest report with the raw status -- NOT a fabricated %SYSTEM-
             * message, exactly as SET PROCESS/NAME does above. In the product
             * this is unreachable (PID 1 refuses to boot without /dev/vms);
             * only the CI negative-control rig can observe it.
             */
            dcl_error("OVMX", 4 /* F */, "SETPRVFAIL",
                      "SET PROCESS/PRIVILEGES could not reach the executive "
                      "(status %%X%08X)", st);
        }
        return (int)st;
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
     * message text instead of leaving the reader to infer it. (SET
     * PROCESS/PRIVILEGES itself is no longer a HIDE stub -- vms-e5d7
     * wired it to the executive-backed sys$setprv -- but SET TIME stays
     * HIDE here because OPER is still outside VMS_PRV_M_ENFORCED: OVMX
     * enforces no priority/time privilege for any account yet, so this
     * gate can grant to none.)
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
 *
 * vms-6f4 Phase 0 (docs/design-dcl-fidelity.md sec 5): SET AUDIT is a named
 * facade -- it used to flip ctx->audit_enabled (a per-process bool no other
 * process, reboot, or real audit trail could observe) and print an "-I-"
 * message while returning SS$_NORMAL. That is INV-DCL's banned fake-success
 * class: the printed text and the numeric status both told the caller the
 * operation succeeded, and nothing did.
 *
 * SET AUDIT is real, privileged VMS syntax (DCL Dictionary), so the honest
 * answer is not a syntax rejection (IVQUAL/IVKEYW) -- the syntax is fine.
 * OVMX has no security-auditing subsystem behind it (that is Phase 2's
 * job), so this reports the genuine VMS "operation not supported" status
 * (SS$_UNSUPPORTED, ssdef.h) and touches no state, rather than claim a
 * toggle that has no effect.
 */
static int cmd_set_audit(struct dcl_command *cmd)
{
    (void)cmd;
    dcl_error("SET", 0, "NOTIMPL",
              "security auditing is not implemented in OVMX - no state changed");
    return SS$_UNSUPPORTED;
}

/*
 * SET ACCOUNTING /ENABLE /DISABLE - Toggle accounting (vms-17d, INV-DCL;
 * docs/design-dcl-fidelity.md sec 5, docs/dcl-verb-fidelity-scoreboard.md
 * "SET ACCOUNTING - moves FACADE to REAL").
 *
 * THE FACADE THIS REPLACES. cmd_set_accounting() used to set
 * ctx->accounting_enabled -- a per-DCL-CONTEXT bool no other process, no
 * later DCL session, and no reboot could observe -- and print
 * "%SET-I-INTSET, accounting enabled/disabled" as if it had. Meanwhile
 * ovmx_accounting_record_login() (login/SSH's accounting writer) ran
 * UNCONDITIONALLY: SET ACCOUNTING controlled nothing. INV-DCL's banned
 * fake-success class exactly.
 *
 * THE FIX. ovmx_accounting_set_enabled()/ovmx_accounting_is_enabled()
 * (src/libvms/rtl/ovmx_accounting.c) persist a REAL, system-wide flag
 * (VMS_ACCOUNTING_STATE_PATH); ovmx_accounting_record_login() now checks
 * it before writing. SHOW ACCOUNTING (dcl_cmd_show.c) reads the SAME flag.
 *
 * Qualifiers per the public OpenVMS DCL Dictionary SET ACCOUNTING entry
 * (<https://wiki.vmssoftware.com/SET_ACCOUNTING>, fetched for this fix):
 * /ENABLE[=(class[,...])] and /DISABLE[=(class[,...])], each keyword
 * naming a resource class (IMAGE, LOGIN_FAILURE, MESSAGE, PRINT, PROCESS)
 * to start/stop tracking. OVMX has no per-class accounting -- only the
 * single system-wide login record ovmx_accounting.c already writes -- so
 * bare /ENABLE and /DISABLE flip that one real flag, and a class list
 * (a value on /ENABLE or /DISABLE) draws the authentic
 * "not implemented" refusal (SS$_UNSUPPORTED) instead of silently
 * accepting granularity nothing honours.
 */
static int cmd_set_accounting(struct dcl_command *cmd)
{
    static const struct dcl_qual_def accounting_quals[] = {
        { "ENABLE",  CDU_VT_LIST, 0, NULL, NULL },
        { "DISABLE", CDU_VT_LIST, 0, NULL, NULL },
        { NULL, 0, 0, NULL, NULL },
    };
    struct dcl_verb accounting_verb_shim = { .quals = accounting_quals };
    uint32_t qstatus = dcl_validate_qualifiers(&accounting_verb_shim, cmd);
    if (qstatus != SS$_NORMAL)
        return (int)qstatus;

    int has_enable = dcl_has_qualifier(cmd, "ENABLE");
    int has_disable = dcl_has_qualifier(cmd, "DISABLE");
    const char *enable_val = dcl_qualifier_value(cmd, "ENABLE");
    const char *disable_val = dcl_qualifier_value(cmd, "DISABLE");

    if ((has_enable && enable_val && enable_val[0]) ||
        (has_disable && disable_val && disable_val[0])) {
        dcl_error("SET", 0, "NOTIMPL",
                  "per-class accounting (/ENABLE=(class,...) or "
                  "/DISABLE=(class,...)) is not implemented in OVMX - "
                  "no state changed");
        return SS$_UNSUPPORTED;
    }

    /* /ENABLE and /DISABLE together (each with no class list) is
     * degenerate for OVMX's single-flag model (real VMS allows both only
     * when their keyword lists don't overlap -- meaningless once neither
     * side names classes). Same precedence the pre-existing facade already
     * had (if/else if): /ENABLE wins, deterministically. */
    if (has_enable) {
        if (ovmx_accounting_set_enabled(1) != 0) {
            dcl_error("OVMX", 2, "WRITEFAIL",
                      "unable to write accounting state - \\%s\\",
                      VMS_ACCOUNTING_STATE_PATH);
            return SS$_BADPARAM;
        }
        printf("%%SET-I-INTSET, accounting enabled\n");
    } else if (has_disable) {
        if (ovmx_accounting_set_enabled(0) != 0) {
            dcl_error("OVMX", 2, "WRITEFAIL",
                      "unable to write accounting state - \\%s\\",
                      VMS_ACCOUNTING_STATE_PATH);
            return SS$_BADPARAM;
        }
        printf("%%SET-I-INTSET, accounting disabled\n");
    } else {
        printf("%%SET-I-INTSET, accounting is %s\n",
               ovmx_accounting_is_enabled() ? "enabled" : "disabled");
    }
    return SS$_NORMAL;
}

/*
 * SET VOLUME - modify characteristics of a mounted vmsfs volume (vms-309).
 *
 * Syntax: SET VOLUME device-name[:]
 *
 * Clean-room (Rule 8): syntax, access requirement, and qualifier list from
 * the public OpenVMS DCL Dictionary SET VOLUME entry
 * (<https://wiki.vmssoftware.com/SET_VOLUME>,
 * <https://www.digiater.nl/openvms/doc/ia64-v8.3/opsys/vmsos83/9996/9996pro_225.html>,
 * fetched for this fix). Syntax is "SET VOLUME device-name[:][,...]" —
 * "the name of one or more mounted Files-11 volumes"; requires ownership
 * or control access to the volume. 22 qualifiers besides /LABEL:
 * /ACCESSED /CACHE /DATA_CHECK /ERASE_ON_DELETE /EXTENSION
 * /FILE_PROTECTION /HIGHWATER_MARKING /LIMIT /LOG /MOUNT_VERIFICATION
 * /OWNER_UIC /PROTECTION /REBUILD /RETENTION /SIZE /STRUCTURE_LEVEL
 * /SUBSYSTEM /UNLOAD /USER_NAME /VOLUME_CHARACTERISTICS /WINDOWS
 * /WRITETHROUGH. /LABEL=volume-label: "Assigns a 1-12 character ANSI name
 * to the volume ... remains in effect until it is changed explicitly;
 * dismounting the volume does not affect the label."
 *
 * THE FACADE THIS REPLACES (docs/dcl-verb-fidelity-scoreboard.md, "SET
 * VOLUME — still open"): cmd_set_volume() used to print "%SET-I-NOTIMPL,
 * SET VOLUME requires a mounted VMSFS volume" and unconditionally return
 * SS$_NORMAL — a success-toned no-op for EVERY invocation, mounted device
 * or not, real qualifier or garbage. INV-DCL's banned class.
 *
 * SCOPE (vms-309): honest errors, not a real /LABEL write-back — vmsfs
 * plumbing for it does not exist. cmd_mount()'s own /LABEL parameter,
 * above, already says as much: "Volume label -- informational only;
 * vmsfs does not read it back." Independently confirmed here:
 * src/kernel/vmsfs/ (the kernel module MOUNT actually mount(2)s) declares
 * no ioctl at all (grep -rn ioctl src/kernel/vmsfs/*.c is empty) — there
 * is no in-kernel path to rewrite hb_volname (vmsfs_ondisk.h) on a volume
 * that is CURRENTLY mounted. Patching the raw block device underneath a
 * live mount from userspace would not be an honest substitute: vmsfs_super.c
 * reads block 1 into sbi->home ONCE at mount and never re-reads it, so an
 * external write would either be invisible to the live mount or get
 * clobbered by the kernel's own cached copy on next write-back — silent
 * corruption, exactly what INV-6/INV-DCL exist to prevent, not an honest
 * refusal. A real /LABEL needs a new vmsfs.ko ioctl (recompute
 * hb_checksum, write through the mount's own buffer head, update
 * sbi->home) — kernel module interface work, CLAUDE.md Design Change
 * Cascade-sized, not a facade-kill patch. Filed as a follow-up (see
 * docs/dcl-verb-fidelity-scoreboard.md's SET VOLUME section).
 *
 * Every qualifier below therefore draws the authentic SS$_UNSUPPORTED
 * refusal instead of the old no-op success — including /LABEL. A bare
 * "SET VOLUME device:" with NO qualifier is not itself dishonest (the
 * Dictionary does not forbid it): it genuinely verifies the device names
 * a mounted volume and changes nothing else, claiming nothing more.
 */
static int cmd_set_volume(struct dcl_command *cmd)
{
    static const struct dcl_qual_def volume_quals[] = {
        { "ACCESSED",               CDU_VT_NONE,  0,                 NULL, NULL },
        { "CACHE",                  CDU_VT_NONE,  CDU_Q_NEGATABLE,   NULL, NULL },
        { "DATA_CHECK",             CDU_VT_LIST,  CDU_Q_NEGATABLE,   NULL, NULL },
        { "ERASE_ON_DELETE",        CDU_VT_NONE,  CDU_Q_NEGATABLE,   NULL, NULL },
        { "EXTENSION",              CDU_VT_VALUE, 0,                 NULL, NULL },
        { "FILE_PROTECTION",        CDU_VT_LIST,  0,                 NULL, NULL },
        { "HIGHWATER_MARKING",      CDU_VT_NONE,  CDU_Q_NEGATABLE,   NULL, NULL },
        { "LABEL",                  CDU_VT_VALUE, CDU_Q_VALREQ,      NULL, NULL },
        { "LIMIT",                  CDU_VT_VALUE, 0,                 NULL, NULL },
        { "LOG",                    CDU_VT_NONE,  CDU_Q_NEGATABLE,   NULL, NULL },
        { "MOUNT_VERIFICATION",     CDU_VT_NONE,  CDU_Q_NEGATABLE,   NULL, NULL },
        { "OWNER_UIC",              CDU_VT_VALUE, 0,                 NULL, NULL },
        { "PROTECTION",             CDU_VT_LIST,  0,                 NULL, NULL },
        { "REBUILD",                CDU_VT_NONE,  CDU_Q_NEGATABLE,   NULL, NULL },
        { "RETENTION",              CDU_VT_VALUE, 0,                 NULL, NULL },
        { "SIZE",                   CDU_VT_VALUE, 0,                 NULL, NULL },
        { "STRUCTURE_LEVEL",        CDU_VT_VALUE, 0,                 NULL, NULL },
        { "SUBSYSTEM",              CDU_VT_LIST,  0,                 NULL, NULL },
        { "UNLOAD",                 CDU_VT_NONE,  0,                 NULL, NULL },
        { "USER_NAME",              CDU_VT_LIST,  0,                 NULL, NULL },
        { "VOLUME_CHARACTERISTICS", CDU_VT_LIST,  0,                 NULL, NULL },
        { "WINDOWS",                CDU_VT_VALUE, 0,                 NULL, NULL },
        { "WRITETHROUGH",           CDU_VT_NONE,  CDU_Q_NEGATABLE,   NULL, NULL },
        { NULL, 0, 0, NULL, NULL },
    };
    struct dcl_verb volume_verb_shim = { .quals = volume_quals };
    uint32_t qstatus = dcl_validate_qualifiers(&volume_verb_shim, cmd);
    if (qstatus != SS$_NORMAL)
        return (int)qstatus;

    /* cmd->params[0] is "VOLUME" (the SET subcommand keyword); [1] is the
     * device name, matching cmd_mount()/cmd_dismount()'s own single-device
     * handling in this same source file (dcl_cmd_misc.c). A comma-separated
     * device LIST (the Dictionary's "device-name[:][,...]") is a disclosed
     * scope limit -- not implemented, same as MOUNT/DISMOUNT. */
    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NODEVICE", "no device specified");
        return SS$_BADPARAM;
    }
    if (cmd->param_count > 2) {
        dcl_error("DCL", 2, "MAXPARM", "too many parameters");
        return SS$_BADPARAM;
    }

    const char *device = cmd->params[1];
    size_t dlen = strlen(device);
    if (dlen < 2 || dlen >= 15) {
        dcl_error("SET", 2, "IVDEVNAM", "invalid device name - \\%s\\", device);
        return SS$_IVDEVNAM;
    }
    char dev_name[16];
    for (size_t i = 0; i < dlen; i++)
        dev_name[i] = (char)toupper((unsigned char)device[i]);
    dev_name[dlen] = '\0';
    if (dev_name[dlen - 1] != ':') {
        dev_name[dlen] = ':';
        dev_name[dlen + 1] = '\0';
    }

    char log_name[16];
    strncpy(log_name, dev_name, sizeof(log_name) - 1);
    log_name[sizeof(log_name) - 1] = '\0';
    size_t lnlen = strlen(log_name);
    if (lnlen > 0 && log_name[lnlen - 1] == ':')
        log_name[lnlen - 1] = '\0';

    char mount_point[64];
    mount_point_for_device(log_name, mount_point, sizeof(mount_point));

    if (!mount_point_is_mounted(mount_point)) {
        /* Dictionary: device-name "specifies the name of one or more
         * MOUNTED Files-11 volumes" -- the same authentic status
         * DISMOUNT already returns for an unmounted target
         * (cmd_dismount(), above in dcl_cmd_misc.c), not the old
         * success-toned NOTIMPL. */
        dcl_error("SET", 2, "DEVNOTMNT", "device is not mounted - _%s", dev_name);
        return SS$_DEVNOTMOUNT;
    }

    /* Mounted. Every SET VOLUME qualifier changes a characteristic OVMX's
     * vmsfs cannot yet persist back to a live volume (see the function
     * header) -- /LABEL included. Report a specific, honest refusal per
     * qualifier given, so the operator knows exactly what did not happen. */
    if (dcl_has_qualifier(cmd, "LABEL")) {
        dcl_error("SET", 0, "NOTIMPL",
                  "SET VOLUME/LABEL is not implemented in OVMX - "
                  "no state changed (vmsfs has no volume-label write-back "
                  "path for a mounted volume)");
        return SS$_UNSUPPORTED;
    }
    static const char *const other_volume_quals[] = {
        "ACCESSED", "CACHE", "DATA_CHECK", "ERASE_ON_DELETE", "EXTENSION",
        "FILE_PROTECTION", "HIGHWATER_MARKING", "LIMIT", "LOG",
        "MOUNT_VERIFICATION", "OWNER_UIC", "PROTECTION", "REBUILD",
        "RETENTION", "SIZE", "STRUCTURE_LEVEL", "SUBSYSTEM", "UNLOAD",
        "USER_NAME", "VOLUME_CHARACTERISTICS", "WINDOWS", "WRITETHROUGH",
        NULL,
    };
    for (int i = 0; other_volume_quals[i]; i++) {
        if (dcl_has_qualifier(cmd, other_volume_quals[i])) {
            dcl_error("SET", 0, "NOTIMPL",
                      "SET VOLUME/%s is not implemented in OVMX - "
                      "no state changed", other_volume_quals[i]);
            return SS$_UNSUPPORTED;
        }
    }

    /* No qualifier given: the device genuinely is a mounted volume and
     * nothing else was asked for -- a real, if pointless, no-op. */
    return SS$_NORMAL;
}

/*
 * SET SYMBOL — control access to local and global symbols in command
 * procedures, and select which symbol-translation context a scope change
 * applies to.
 *
 * OpenVMS DCL Dictionary (VSI OpenVMS DCL Dictionary N–Z, SET SYMBOL) defines
 * EXACTLY four command qualifiers:
 *
 *   /SCOPE=(keyword,...)  Controls access to local and global symbols. Keywords:
 *       NOLOCAL  - local symbols defined in OUTER command levels are treated as
 *                  undefined by the current and all inner command levels.
 *       LOCAL    - removes any local translation limit set at this level.
 *       NOGLOBAL - global symbols are inaccessible to the current and inner
 *                  command levels.
 *       GLOBAL   - restores access to all global symbols.
 *   /ALL      (default)  The /SCOPE values apply BOTH to translation of the
 *                        first token on a command line and to general symbol
 *                        substitution. Incompatible with /GENERAL and /VERB.
 *   /GENERAL             The /SCOPE values apply to all symbols EXCEPT the first
 *                        token on a command line. Incompatible with /ALL, /VERB.
 *   /VERB                The /SCOPE values apply ONLY to translation of the
 *                        first token on a command line as a symbol before
 *                        processing. Incompatible with /ALL and /GENERAL.
 *
 * NOTE (clean-room, vms-c211): the qualifiers /GLOBAL and /LOCAL do NOT exist
 * on SET SYMBOL — LOCAL/GLOBAL are /SCOPE *keywords*, not qualifiers. Real DCL
 * rejects "SET SYMBOL/GLOBAL" with %DCL-W-IVQUAL, which is what OVMX does here.
 */
static int cmd_set_symbol(struct dcl_command *cmd)
{
    /* The complete SET SYMBOL qualifier set. Any parsed qualifier that is not a
     * unique (case-insensitive) prefix of one of these is unknown to VMS and
     * draws an authentic %DCL-W-IVQUAL (INV-DCL: honest error, never a silent
     * accept). Abbreviations resolve the VMS way — exact wins, else a unique
     * prefix; an ambiguous prefix is treated as unknown. */
    static const char *const set_symbol_quals[] = {
        "SCOPE", "ALL", "GENERAL", "VERB", NULL
    };

    int have_scope = 0, have_all = 0, have_general = 0, have_verb = 0;
    const char *scope_val = NULL;

    for (int i = 0; i < cmd->qualifier_count; i++) {
        const char *qn = cmd->qualifiers[i].name;
        if (!cmd->qualifiers[i].present || !qn[0])
            continue;

        /* Resolve qn against the legal set (exact, else unique prefix). */
        const char *canon = NULL;
        int hits = 0;
        size_t qlen = strlen(qn);
        for (const char *const *k = set_symbol_quals; *k; k++) {
            if (strcasecmp(*k, qn) == 0) { canon = *k; hits = 1; break; }
            if (strncasecmp(*k, qn, qlen) == 0) { canon = *k; hits++; }
        }

        /* Unknown qualifier, ambiguous prefix, or a /NO form of a qualifier
         * that has no negated meaning (/NOSCOPE, /NOVERB, ...): %DCL-W-IVQUAL. */
        if (hits != 1 || cmd->qualifiers[i].negated) {
            dcl_error("DCL", 0, "IVQUAL",
                "unrecognized qualifier - check validity, spelling, and "
                "placement\n \\%s\\", qn);
            return SS$_IVQUAL;
        }

        if (strcmp(canon, "SCOPE") == 0) {
            have_scope = 1;
            scope_val = cmd->qualifiers[i].value;
        } else if (strcmp(canon, "ALL") == 0)     have_all = 1;
        else if (strcmp(canon, "GENERAL") == 0)   have_general = 1;
        else /* VERB */                           have_verb = 1;
    }

    /* /ALL, /GENERAL and /VERB are mutually exclusive (DCL Dictionary). */
    if (have_all + have_general + have_verb > 1) {
        dcl_error("DCL", 0, "CONFLICT",
            "conflicting qualifiers - /ALL, /GENERAL and /VERB may not be "
            "combined");
        return SS$_ABORT;
    }

    int domain = DCL_SYM_DOMAIN_ALL;           /* /ALL is the default */
    if (have_verb)         domain = DCL_SYM_DOMAIN_VERB;
    else if (have_general) domain = DCL_SYM_DOMAIN_GENERAL;

    if (have_scope) {
        int hide_local = 0, hide_global = 0;   /* default: both accessible */
        if (scope_val && *scope_val) {
            char buf[256];
            strncpy(buf, scope_val, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            for (char *t = strtok(buf, "(), \t");
                 t; t = strtok(NULL, "(), \t")) {
                if (strcasecmp(t, "NOLOCAL") == 0)       hide_local = 1;
                else if (strcasecmp(t, "LOCAL") == 0)    hide_local = 0;
                else if (strcasecmp(t, "NOGLOBAL") == 0) hide_global = 1;
                else if (strcasecmp(t, "GLOBAL") == 0)   hide_global = 0;
                else {
                    dcl_error("DCL", 2, "IVKEYW",
                        "unrecognized SET SYMBOL/SCOPE keyword - \\%s\\", t);
                    return SS$_IVKEYW;
                }
            }
        }

        dcl_sym_scope_set(hide_local, hide_global, domain);
        return SS$_NORMAL;
    }

    /* No /SCOPE: /ALL, /GENERAL and /VERB only qualify a /SCOPE value, so with
     * nothing to apply this is a valid no-op (as is a bare "SET SYMBOL"). VMS
     * accepts it without error; changing no state is the honest result. */
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
        /* SET NOON — disable $STATUS checking at THIS command level: DCL then
         * performs neither the default exit-on-error nor any armed ON action,
         * so the procedure continues past errors. Clean-room (Rule 8): VSI
         * OpenVMS DCL Dictionary, "SET ON"/"SET NOON". */
        struct dcl_context *c = dcl_get_context();
        if (c->proc_depth >= 0)
            c->proc_stack[c->proc_depth].noon = 1;
        else
            c->noon_active = 1;
        return SS$_NORMAL;
    }
    if (dcl_match_command(subcmd, "ON", 2)) {
        /* SET ON — restore $STATUS checking (the default) at this command
         * level, re-enabling both the default error-stop and any ON action. */
        struct dcl_context *c = dcl_get_context();
        if (c->proc_depth >= 0)
            c->proc_stack[c->proc_depth].noon = 0;
        else
            c->noon_active = 0;
        return SS$_NORMAL;
    }
    if (dcl_match_command(subcmd, "SYMBOL", 3))
        return cmd_set_symbol(cmd);
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
