/*
 * dcl_main.c - DCL Shell Main Loop
 *
 * This is the primary entry point for the OVMX DCL shell.
 * Implements the REPL (Read-Eval-Print Loop) with VMS-style
 * prompt, command line editing, history, and symbol substitution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <termios.h>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/context.h"
#include "dcl/dcl_cmd.h"
#include "dcl/terminal.h"
#include "dcl/cdu.h"
#include "dcl/dcl_mbx.h"
#include "ssdef.h"
#include "vms/pcb.h"
#include "ovmx_identity.h"
#include "vms/privs.h"
#include "vms/logical.h"
#include "sysuaf.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
/* The executive process table: this process's identity is READ from it. */
#include "vms_kif.h"

/* Global DCL context */
static struct dcl_context dcl_ctx;

struct dcl_context *dcl_get_context(void)
{
    return &dcl_ctx;
}

/* Forward declarations */
int dcl_execute_line(const char *line);
int dcl_execute_script(const char *filename, int argc, char **argv);
int dcl_format_directory(const char *linux_path, char *vms_dir, size_t dir_size);
void dcl_set_status(struct dcl_context *ctx, int status);

/* Login script paths */
#include "ovmx_layout.h"
#define SYLOGIN_PATH  VMS_SYLOGIN_PATH

/* parse_privilege_string() is now in vms/privs.h (shared header) */

/*
 * Initialize DCL context with defaults.
 */
void dcl_context_init(struct dcl_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    strcpy(ctx->prompt, "$ ");
    ctx->interactive = isatty(STDIN_FILENO);
    ctx->ctrl_y_enabled = 1;
    ctx->last_status = SS$_NORMAL;
    ctx->last_severity = 1;
    ctx->proc_depth = -1;
    ctx->if_depth = 0;
    ctx->gosub_depth = 0;

    /* SET MESSAGE defaults: all flags on */
    ctx->msg_facility = 1;
    ctx->msg_severity = 1;
    ctx->msg_ident    = 1;
    ctx->msg_text     = 1;

    /* SET TERMINAL defaults — full characteristics model */
    vms_terminal_init(&ctx->terminal);

    /*
     * DELETED, NOT REPLACED (vms-fb9): the terminal-identity handoff.
     *
     * Three lines used to stand here. getenv("VMS_DEVICE_TYPE") and
     * getenv("VMS_TERMINAL") took this process's terminal identity out
     * of its own environment -- set by src/ovmx_init/ovmx_init.c for the
     * console and by src/vmsssh/vmssshd.c for a remote session -- and
     * failing that, vms_term_allocate("_FTA", ...) handed this process a
     * name out of a private pool file. All three are the VMS_PRCNAM
     * shape the operator rejected on 2026-07-30 (CLAUDE.md rule 10,
     * worked example 2): a process telling itself what it is, in a way
     * no other process can see or contradict. They compiled, they
     * tested green, and they were a lie.
     *
     * They are NOT replaced with a better guess. On VMS the answer comes
     * from the executive -- the job's terminal is recorded in the
     * executive's process database and the device itself lives in the
     * executive's device table (src/kernel/vms_devtab.c has the device
     * half as of vms-d0b; the process half does not exist yet). Until a
     * process can ASK the executive which terminal it is on, DCL does
     * not have that answer, so ctx->terminal.device_name stays empty and
     * SHOW TERMINAL prints no name rather than an invented one (rule 10:
     * hide what cannot be answered faithfully; never fabricate).
     *
     * Do not "fix" this by reintroducing an environment variable, a
     * pool, an isatty() guess or a ttyname() translation. The fix is the
     * executive-resident process/terminal binding; anything else is this
     * same defect wearing a different name.
     */

    /* SET PROCESS defaults */
    ctx->process_priority = 4;   /* Default VMS base priority */

    /* Set default directory from environment (VMS spec) or derive from cwd */
    const char *env_defdir_init = getenv("VMS_DEFAULT_DIR");
    if (env_defdir_init && env_defdir_init[0]) {
        strncpy(ctx->default_dir, env_defdir_init, sizeof(ctx->default_dir) - 1);
        ctx->default_dir[sizeof(ctx->default_dir) - 1] = '\0';
    } else {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            vmsfs_to_vms_spec(cwd, ctx->default_dir, sizeof(ctx->default_dir));
        }
        /* Fall back if cwd could not be obtained or translated */
        if (ctx->default_dir[0] == '\0') {
            strcpy(ctx->default_dir, "SYS$SYSDEVICE:[000000]");
        }
    }

    /*
     * DELETED WITH ITS SOURCE (vms-fb9): this used to copy
     * ctx->terminal.device_name into ctx->process_name -- deriving the
     * process name from the terminal name the block above had just
     * invented. With the handoff gone there is nothing to derive it
     * from, and inventing a second name to stand in for the first would
     * be the same defect twice. The process name is executive-owned
     * state; see src/vmsprocess/vms_pcb.c, which is still a per-process
     * block and is tracked separately.
     *
     * ALSO DELETED (vms-2b8, this round): the "Get user info" block that
     * stood here, a getpwuid(getuid()) fallback for ctx->username used
     * whenever VMS_USERNAME was unset. It is superseded below by the
     * executive read -- a local Linux passwd lookup is exactly the kind
     * of locally-invented identity Rule 10 forbids once an authoritative
     * source (the executive) exists.
     */


    /*
     * ============================================================
     * IDENTITY COMES FROM THE EXECUTIVE (vms-2b8)
     * ============================================================
     * This process's USER NAME, UIC and PRIVILEGE MASK are read out of
     * the executive's process table (src/kernel/vms_proctab.c, through
     * vms_kif_getjpi_self). They are not asked for, and there is no
     * other source for them.
     *
     * WHAT USED TO STAND HERE, so nobody puts it back: the user name,
     * the UIC and the privilege mask were taken from the environment
     * (VMS_USERNAME / VMS_UIC_GROUP / VMS_UIC_MEMBER / VMS_PRIVILEGES),
     * with the user name falling back to getpwuid(getuid()) and then to
     * the literal "SYSTEM". Every one of those is a value the process
     * itself controls -- any process could setenv("VMS_PRIVILEGES",
     * "ALL") and be believed. That is the env-var facade CLAUDE.md
     * Rule 10 names as a worked example; it was never an access control
     * system, it was an honor system.
     *
     * The executive derives the UIC from the task's real credentials and
     * the authorized mask from capable(CAP_SYS_ADMIN) -- credentials a
     * process cannot grant itself -- and the user name arrives only
     * through VMS_IOCTL_SETIDENT, which refuses any caller without
     * SETPRV an identity that is not a weakening of its own. So what is
     * read here is what something privileged established for this
     * process, which is the whole difference between an identity and a
     * claim (CLAUDE.md Rule 11).
     *
     * THERE IS NO ABSENT-EXECUTIVE BRANCH AND MUST NOT BE ONE. The
     * first vms_kif_* call opens and registers this process (kif_bind),
     * and PID 1 refuses to bring OVMX up at all without /dev/vms
     * (Rule 9, src/ovmx_init/ovmx_init.c executive_attach), so in the
     * one OVMX runtime this read cannot fail. If it fails anyway -- on
     * a developer host running the DCL binary outside OVMX -- the
     * fields stay empty and every reader of them reports nothing.
     * Substituting a local guess for a failed executive read is exactly
     * the illegal third answer (Rule 10).
     * ============================================================
     */
    struct vms_procinfo self;
    memset(&self, 0, sizeof(self));
    if (vms_kif_getjpi_self(&self) & 1) {
        strncpy(ctx->username, self.username, sizeof(ctx->username) - 1);
        ctx->username[sizeof(ctx->username) - 1] = '\0';
        ctx->uic_group  = (self.uic >> 16) & 0xFFFFu;
        ctx->uic_member = self.uic & 0xFFFFu;
        ctx->privileges = self.cur_privs;
    }

    const char *env_defdir = getenv("VMS_DEFAULT_DIR");
    if (env_defdir && env_defdir[0]) {
        strncpy(ctx->default_dir, env_defdir, sizeof(ctx->default_dir) - 1);
        ctx->default_dir[sizeof(ctx->default_dir) - 1] = '\0';
    }

    /* Default protection: S:RWED,O:RWED,G:RE,W: = 0xFF00 */
    ctx->default_protection = 0xFF00;

    /*
     * The userspace PCB is seeded from the executive's row -- it is a
     * copy of what the executive decided, never a declaration of what
     * this process would like to be. (The PCB itself is a per-process
     * structure and therefore a facade for anything system-wide; it is
     * vms-8019's to remove, not this item's. What this item removes is
     * the process CHOOSING what goes in it.)
     */
    if (!vms_pcb_get()) {
        struct vms_pcb *pcb = vms_pcb_init(self.cur_privs);
        if (pcb) {
            vms_pcb_set_identity(self.vms_pid, self.uic,
                                 ctx->username, ctx->process_name);
            if (env_defdir && env_defdir[0])
                vms_pcb_set_default_dir(env_defdir);
        }
    }
}

/*
 * Signal handler for Ctrl-C / Ctrl-Y (VMS interrupt model).
 *
 * When a child process is running:
 *   - If Ctrl-Y is enabled, stop the child (SIGTSTP) and return to DCL prompt
 *   - The stopped child PID is saved so CONTINUE can resume it
 * When no child is running:
 *   - Cancel the current input line (standard shell behavior)
 */
static volatile sig_atomic_t sigint_received = 0;

/* Currently running child PID — set during waitpid, cleared after.
 * Accessed from signal handler, so must be volatile sig_atomic_t. */
volatile sig_atomic_t dcl_running_child = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    sigint_received = 1;

    pid_t child = (pid_t)dcl_running_child;
    if (child > 0 && dcl_ctx.ctrl_y_enabled) {
        /* Stop the running child process — VMS Ctrl-Y behavior */
        kill(child, SIGTSTP);
        /* The waitpid in the caller (with WUNTRACED) will pick up the stop */
        return;
    }

    /* No child running — cancel current input line */
#ifdef HAVE_READLINE
    printf("\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
#else
    printf("\n");
#endif
}

/*
 * Ctrl+C handler — forwards signal to running child (VMS user-mode AST).
 * Unlike Ctrl+Y, this does NOT return to DCL prompt.
 */
static void ctrl_c_handler(int sig)
{
    (void)sig;
    pid_t child = (pid_t)dcl_running_child;
    if (child > 0) {
        kill(child, SIGINT);
    }
}

/*
 * dcl_emit_ctrl_t_status - the reflexive Ctrl/T handler.
 *
 * VMS prints a one-line process status when the user presses Ctrl/T, if
 * Ctrl/T is enabled (SET CONTROL=T; disabled by default). This is the
 * handler the input layer invokes on a Ctrl/T (0x14) keystroke.
 *
 * The whole line is sourced from the executive's $GETJPI row --
 * vms_kif_getjpi_self(), the SAME reader SHOW PROCESS uses -- and rendered
 * by dcl_format_ctrl_t_status() (dcl_terminal.c), which fabricates nothing:
 * see its citation and per-field sourcing. If the executive is absent (no
 * /dev/vms) $GETJPI fails and we emit NOTHING rather than a faked line
 * (INV-6 / CLAUDE.md Rule 9).
 *
 * DEFERRED GAP (vms-0d75 follow-up): the image-name field is empty because
 * OVMX does not yet source JPI$_IMAGNAME for a running image, and the "IO="
 * token is omitted because JPI$_DIRIO/BUFIO carry no faithful OVMX source
 * (their fields_valid bits are never set). Both are honest omissions, not
 * fabrications; filed as a follow-up rather than filled with plausible
 * numbers.
 *
 * Returns 1 if a status line was emitted, 0 otherwise (disabled or no
 * executive) so the caller knows whether the display needs a refresh.
 */
static int dcl_emit_ctrl_t_status(void)
{
    struct dcl_context *ctx = dcl_get_context();
    if (!ctx->ctrl_t_enabled)
        return 0;   /* SET NOCONTROL=T (VMS default): Ctrl/T is inert */

    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    if (!(vms_kif_getjpi_self(&info) & 1))
        return 0;   /* no executive -> no line, never a fabricated one */

    char node[64];
    ovmx_node_name(node, sizeof(node));

    char line[256];
    /* No image name: none is activated at the DCL prompt (see the gap note
     * above). JPI$_IMAGNAME would be sourced here once OVMX tracks it. */
    if (dcl_format_ctrl_t_status(&info, node, "", time(NULL),
                                 line, sizeof(line)) & 1) {
        fputs("\r\n", stdout);
        fputs(line, stdout);
        fputs("\r\n", stdout);
        fflush(stdout);
        return 1;
    }
    return 0;
}

#ifdef HAVE_READLINE
/*
 * Readline binding for Ctrl/T (0x14). Readline's emacs default maps 0x14 to
 * transpose-chars; this rebind gives it VMS Ctrl/T semantics instead. The
 * status line is emitted between the current input line and a fresh redraw
 * of it, so the reflexive line "momentarily interrupts" without disturbing
 * what the user has typed (OpenVMS User's Manual). When Ctrl/T is disabled
 * the keystroke is simply swallowed.
 */
static int dcl_ctrl_t_rl_handler(int count, int key)
{
    (void)count;
    (void)key;
    if (dcl_emit_ctrl_t_status()) {
        rl_on_new_line();
        rl_forced_update_display();
    }
    return 0;
}
#endif

/*
 * Terminal configuration for VMS signal/EOF model.
 * - VEOF = 26 (Ctrl+Z is EOF, not Ctrl+D)
 * - VINTR = 25 (Ctrl+Y generates SIGINT for DCL interrupt)
 * - VQUIT = 3 (Ctrl+C generates SIGQUIT for user-mode AST)
 * - VSUSP = disabled (Ctrl+Z is EOF, not suspend)
 */
static struct termios orig_termios;
static int termios_saved = 0;

static void restore_termios(void)
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        termios_saved = 0;
    }
}

static void setup_vms_eof(void)
{
    if (!isatty(STDIN_FILENO))
        return;

    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &tio) != 0)
        return;

    /* Save original settings for restore on exit */
    orig_termios = tio;
    termios_saved = 1;
    atexit(restore_termios);

    /* Set VEOF to Ctrl+Z (0x1A = 26) — VMS convention */
    tio.c_cc[VEOF] = 26;
    /* Disable Ctrl+Z as suspend (we use it for EOF) */
    tio.c_cc[VSUSP] = _POSIX_VDISABLE;
    /* Map Ctrl+Y (0x19) to VINTR — generates SIGINT for DCL interrupt */
    tio.c_cc[VINTR] = 25;
    /* Map Ctrl+C (0x03) to VQUIT — generates SIGQUIT for user-mode AST */
    tio.c_cc[VQUIT] = 3;
    tcsetattr(STDIN_FILENO, TCSANOW, &tio);
}

/*
 * The DCL-side login banner + last-login emitter was DELETED here (vms-417,
 * folds vms-46b).
 *
 * It was a SECOND, DIVERGENT emitter of the login sequence LOGINOUT already
 * owns (tools/vms_login.c -> loginout_display.c): a different banner line
 * ("<product> on node <n> <date>") and a differently-formatted, differently
 * -sourced "Last interactive login" line that read the raw accounting file
 * text instead of a formatted timestamp. Two emitters of one VMS surface is
 * exactly the drift the fidelity work exists to kill -- on the real runtime
 * DCL is always started by LOGINOUT (--login), which prints the authentic
 * sequence, and this ran only on the never-taken "vmsdcl interactive, no
 * --login" path. LOGINOUT is the one emitter.
 */

/*
 * Set up default logical names and symbols for the session.
 */
static void setup_session(struct dcl_context *ctx)
{
    /* Initialize symbol tables */
    dcl_sym_init();

    /* Initialize the LNM manager and populate default system logicals */
    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        const char *vms_root = getenv("VMS_ROOT");
        if (!vms_root) vms_root = SYSDISK_MOUNT;

        /* Register the system disk in the device table before LNM setup.
         * DKA0: → vms_root is the ONE place a Unix path enters the namespace. */
        vmsfs_device_add("DKA0", vms_root);

        lnm_setup_defaults(mgr, vms_root);

        /*
         * SYS$NODE -- the local node name as a system logical (vms-f89).
         * On OpenVMS F$TRNLNM("SYS$NODE") returns the node name in DECnet
         * full-name form, i.e. with the "::" node delimiter appended (the
         * common idiom NODE = F$TRNLNM("SYS$NODE") - "::" relies on it);
         * VSI OpenVMS DCL Dictionary, F$GETSYI / SYS$NODE. Sourced from the
         * identity SSOT (ovmx_node_name -> SYSGEN SCSNODE), so it tracks the
         * configured cluster identity rather than a literal, and a later
         * DEFINE/SYSTEM SYS$NODE overrides it live. Seeded into LNM$SYSTEM at
         * runtime (node-wide, via the executive); absent /dev/vms the executive
         * refuses with SS$_NOSUCHDEV and we fall back to LNM$PROCESS_TABLE --
         * the same disclosed process-scope fallback lnm_seed_system_locating()
         * uses -- so F$TRNLNM still resolves it in a host DCL session. Marked
         * terminal: the node name is a literal, not subject to further
         * translation.
         */
        char node[OVMX_IDENTITY_MAXLEN];
        ovmx_node_name(node, sizeof(node));
        char nodeval[OVMX_IDENTITY_MAXLEN + 3];
        snprintf(nodeval, sizeof(nodeval), "%s::", node);
        uint32_t node_st = lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$NODE",
                                      nodeval, LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
        if (node_st == SS$_NOSUCHDEV)
            lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$NODE", nodeval,
                       LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

        /*
         * SYS$TIMEZONE_* from the live system TZ config (vms-f89). On OpenVMS
         * these are system logicals a boot procedure defines from the chosen
         * zone; date/time formatting reads SYS$TIMEZONE_DIFFERENTIAL at point
         * of use (lib_datetime.c), so a later DEFINE takes effect live.
         *
         * Doc: VSI OpenVMS System Manager's Manual, Vol. 1, "Managing the
         * System Time" -- SYS$TIMEZONE_NAME (the local zone name),
         * SYS$TIMEZONE_RULE (the standard/daylight changeover rule),
         * SYS$TIMEZONE_DIFFERENTIAL (the time differential factor, seconds from
         * UTC). tm_gmtoff is that differential (seconds east of UTC), the
         * current TDF including any daylight offset in effect -- exactly what
         * VMS keeps as a single fixed differential. Same SYSTEM-then-PROCESS
         * seeding as SYS$NODE. Identity stays OVMX; only the clock's zone is
         * described.
         *
         * No explicit tzset(): localtime_r resolves the zone (and fills
         * tm_gmtoff / tm_zone) internally on both glibc and musl, and tzset is
         * NOT one of the DECC$SHR universals the VMS-native LINK.EXE graph
         * resolves against (it is not in mk_decc_shr.sh's vector) -- calling it
         * broke the native DCL.EXE link. time / localtime_r ARE universals.
         */
        {
            time_t tznow = time(NULL);
            struct tm lt;
            localtime_r(&tznow, &lt);

            char tzval[128];

            snprintf(tzval, sizeof(tzval), "%ld", (long)lt.tm_gmtoff);
            if (lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$TIMEZONE_DIFFERENTIAL",
                           tzval, LNM_ATTR_TERMINAL, LNM_MODE_EXEC)
                    == SS$_NOSUCHDEV)
                lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$TIMEZONE_DIFFERENTIAL",
                           tzval, LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

            const char *tz_env = getenv("TZ");
            const char *zname = (lt.tm_zone && lt.tm_zone[0]) ? lt.tm_zone
                                : (tz_env && tz_env[0]) ? tz_env : "UTC";
            if (lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$TIMEZONE_NAME",
                           zname, LNM_ATTR_TERMINAL, LNM_MODE_EXEC)
                    == SS$_NOSUCHDEV)
                lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$TIMEZONE_NAME",
                           zname, LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

            /* The rule is the POSIX TZ changeover string; getenv("TZ") IS that
             * string when the site set it, else a fixed-offset standard-only
             * rule from the zone name (honest: no daylight rule is fabricated
             * where the system config supplies none). */
            if (tz_env && tz_env[0]) {
                strncpy(tzval, tz_env, sizeof(tzval) - 1);
                tzval[sizeof(tzval) - 1] = '\0';
            } else {
                snprintf(tzval, sizeof(tzval), "%s0", zname);
            }
            if (lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$TIMEZONE_RULE",
                           tzval, LNM_ATTR_TERMINAL, LNM_MODE_EXEC)
                    == SS$_NOSUCHDEV)
                lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$TIMEZONE_RULE",
                           tzval, LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
        }

        /*
         * Override SYS$DISK with the process default device.
         * On a VMS system this would be set from the process PCB;
         * here we default to the system device.
         */
        lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$DISK",
                   "SYS$SYSDEVICE", 0, LNM_MODE_USER);
    }

    /* Register built-in commands */
    dcl_register_builtins();

    /* Set initial special symbols. $STATUS is rendered VMS-style ("%X00000001"
     * for SS$_NORMAL), not decimal -- see dcl_set_status(). */
    dcl_set_status(ctx, SS$_NORMAL);
    dcl_sym_set("$RESTART", "FALSE", DCL_SYM_GLOBAL);

    /* Set user symbol */
    dcl_sym_set("$USER", ctx->username, DCL_SYM_GLOBAL);

    /* Set P1-P8 to empty */
    for (int i = 1; i <= 8; i++) {
        char pname[4];
        snprintf(pname, sizeof(pname), "P%d", i);
        dcl_sym_set(pname, "", DCL_SYM_LOCAL);
    }
}

/*
 * Process a single line of DCL input.
 * Handles symbol substitution, comment stripping, and dispatch.
 */
static int process_line(struct dcl_context *ctx, char *line)
{
    if (!line) return -1;

    /* Skip leading whitespace */
    while (*line && isspace((unsigned char)*line)) line++;

    /* Skip empty lines */
    if (*line == '\0') return 0;

    /* Handle leading $ (as in command procedures) */
    if (*line == '$') {
        line++;
        while (*line && (*line == ' ' || *line == '\t')) line++;
    }

    /* Skip empty and comment lines */
    if (*line == '\0') return 0;
    if (*line == '!') return 0;

    /* Perform symbol substitution */
    char substituted[DCL_MAX_LINE];
    dcl_sym_substitute(line, substituted, sizeof(substituted));

    /* Execute the command. $STATUS/$SEVERITY are refreshed inside
     * dcl_execute_command() for every real command (including one that was not
     * found); control-flow verbs (IF/GOTO/assignment/...) deliberately leave
     * $STATUS untouched so `IF .NOT. $STATUS` can still see the failing
     * command's status -- so process_line() must NOT re-stamp $STATUS here, or
     * it would clobber that value with the control command's success. */
    int status = dcl_execute_line(substituted);
    return status;
}

/*
 * Main REPL loop.
 */
int main(int argc, char *argv[])
{
    /* Initialize context */
    dcl_context_init(&dcl_ctx);
    setup_session(&dcl_ctx);

    /* Establish and register DCL's P1 control region (vms-68f.v). This lays
     * the process-permanent control block whose extent $GETJPI reports and
     * whose critical page imgact_activate() protects while an in-process
     * image runs -- the wiring vms_kif_p1_map had been waiting for since
     * increment (ii). Best-effort: absent /dev/vms it registers nothing and
     * DCL runs unchanged (INV-6, no per-process fake). */
    dcl_p1_init();

    /* Set VEOF to Ctrl+Z (VMS convention) */
    setup_vms_eof();

    /* Check for --login flag, and the login command file LOGINOUT hands over
     * with --lgicmd <spec> (vms-e48: the SYSUAF LGICMD field, already resolved
     * to its documented default by LOGINOUT when the field was empty). */
    int login_mode = 0;
    const char *login_lgicmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--login") == 0) {
            login_mode = 1;
            dcl_ctx.logged_in = 1;
        } else if (strcmp(argv[i], "--lgicmd") == 0 && i + 1 < argc) {
            login_lgicmd = argv[++i];
        }
    }

    /* Check for -c "command" mode */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        dcl_ctx.interactive = 0;
        int status = dcl_execute_line(argv[2]);
        dcl_sym_cleanup();
        return (status & 1) ? 0 : 1;
    }

    /* Check for script mode: vmsdcl script.com [params...] */
    if (argc > 1 && argv[1][0] != '-' && !login_mode) {
        dcl_ctx.interactive = 0;
        int status = dcl_execute_script(argv[1], argc - 2, argv + 2);
        dcl_sym_cleanup();
        return (status & 1) ? 0 : 1;
    }

    /*
     * MAILBOX-DRIVEN SYS$INPUT/SYS$OUTPUT (vms-786). A persistent DCL a parent
     * drives over mailboxes -- MMK's send_cmd_and_wait, which feeds command
     * lines down one mailbox and reads results back over another
     * (docs/design-mmk-exec-drive-ovmx.md) -- has SYS$INPUT/SYS$OUTPUT bound to
     * mailbox devices (MBAn:), not a terminal or a file. If those logical names
     * translate to a mailbox, bind DCL's command-read loop to read commands via
     * $QIO IO$_READVBLK and its output to leave via IO$_WRITEVBLK (real
     * executive I/O, Rule 9). This is an ADDED source: for a terminal or
     * @-procedure SYS$INPUT it binds nothing and the fd/stdio path below is
     * unchanged. Placed AFTER the -c and script early-returns so only the
     * persistent-REPL invocation consults it. */
    int mbx_bound = dcl_mbx_bind_std_streams();
    if (mbx_bound & DCL_MBX_BOUND_INPUT)
        dcl_ctx.interactive = 0;   /* a mailbox is never an interactive TTY */

    /* Interactive mode */
    if (dcl_ctx.interactive) {
        /* No banner here: LOGINOUT (tools/vms_login.c) is the ONE emitter of
         * the login sequence (vms-417). The old DCL-side banner call was
         * removed with its function. */

        /* Set up signal handling */
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigint_handler;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);

        /* Ctrl+C → SIGQUIT: forward to child as user-mode AST */
        struct sigaction sa_quit;
        memset(&sa_quit, 0, sizeof(sa_quit));
        sa_quit.sa_handler = ctrl_c_handler;
        sa_quit.sa_flags = 0;
        sigemptyset(&sa_quit.sa_mask);
        sigaction(SIGQUIT, &sa_quit, NULL);

#ifdef HAVE_READLINE
        /* Set up readline */
        rl_readline_name = "DCL";
        using_history();
        /* Bind Ctrl-B to previous-history (VMS-style recall key) */
        rl_bind_key(2, rl_get_previous_history);
        /* Bind Ctrl-T (0x14) to the reflexive status-line handler, replacing
         * readline's default transpose-chars. Gated on SET CONTROL=T. */
        rl_bind_key(20, dcl_ctrl_t_rl_handler);
#endif
    }

    /* Execute login scripts when in login mode */
    if (login_mode) {
        struct stat st;

        /*
         * ESTABLISH THE PER-USER IDENTITY LOGICALS (vms-e48).
         *
         * SYS$LOGIN and SYS$LOGIN_DEVICE become REAL LNM$JOB logicals, sourced
         * from the SYSUAF default device/directory that LOGINOUT read and
         * conveyed through VMS_DEFAULT_DIR (which dcl_context_init already
         * copied into ctx->default_dir). This is what makes F$TRNLNM("SYS$LOGIN")
         * return this user's real home instead of the generic
         * SYS$SYSDEVICE:[USERS] default. (SYS$SCRATCH is left at OVMX's
         * system-wide [SYSTMP] scratch -- see lnm_define_login_logicals().)
         *
         * LNM$JOB, NOT LNM$PROCESS (round 2). These are JOB-wide names on VMS
         * so the WHOLE login job agrees on them -- including the images DCL
         * activates (a foreign command like `$ PARTS`, which DCL fork+execs).
         * A process-scope value here would be invisible to that child. The
         * executive keys LNM$JOB on the job tree and a forked child inherits
         * the parent's job_id, so the child resolves it identically. It must
         * happen BEFORE the login command procedures run, so their F$TRNLNM /
         * SYS$LOGIN:-relative filespecs resolve against the real home.
         */
        lnm_manager_t *login_mgr = lnm_get_manager();
        if (login_mgr && dcl_ctx.default_dir[0])
            lnm_define_login_logicals(login_mgr, LNM_JOB_TABLE,
                                      dcl_ctx.default_dir);

        /* System-wide login script */
        char sylogin_linux[1024];
        vmsfs_to_linux_path(SYLOGIN_PATH, sylogin_linux, sizeof(sylogin_linux));
        if (stat(sylogin_linux, &st) == 0 && S_ISREG(st.st_mode)) {
            dcl_execute_script(sylogin_linux, 0, NULL);
        }

        /*
         * Per-user login command file: the SYSUAF LGICMD field LOGINOUT handed
         * over via --lgicmd, or the documented SYS$LOGIN:LOGIN.COM default
         * (vms-e48). Resolved through vmsfs_to_linux_path(), which translates
         * the SYS$LOGIN logical just defined above -- NOT getenv("SYS$LOGIN")
         * with a hardcoded "/LOGIN.COM" tail, and NOT the SYSUAF field ignored.
         */
        const char *lgicmd_spec =
            (login_lgicmd && login_lgicmd[0]) ? login_lgicmd
                                              : SYSUAF_DEFAULT_LGICMD;
        char user_login[1024];
        if (vmsfs_to_linux_path(lgicmd_spec, user_login, sizeof(user_login)) == 1
            && stat(user_login, &st) == 0 && S_ISREG(st.st_mode)) {
            dcl_execute_script(user_login, 0, NULL);
        }
    }

    /* Main REPL */
    while (!dcl_ctx.exit_requested && !dcl_ctx.logout_requested) {
        char *line = NULL;

        sigint_received = 0;

        if (dcl_ctx.interactive) {
#ifdef HAVE_READLINE
            line = readline(dcl_ctx.prompt);
            if (!line) {
                /* EOF (Ctrl-Z on VMS) — treat as LOGOUT */
                printf("\n");
                break;
            }
            /* Add to history if non-empty */
            if (line[0] != '\0') {
                add_history(line);
            }
#else
            /* No readline - use fgets */
            static char fgets_buf[DCL_MAX_LINE];
            printf("%s", dcl_ctx.prompt);
            fflush(stdout);
            if (!fgets(fgets_buf, sizeof(fgets_buf), stdin)) {
                printf("\n");
                break;
            }
            size_t flen = strlen(fgets_buf);
            if (flen > 0 && fgets_buf[flen - 1] == '\n')
                fgets_buf[flen - 1] = '\0';
            line = strdup(fgets_buf);
            if (!line) break;
#endif
        } else {
            /* Non-interactive: read from stdin */
            static char buf[DCL_MAX_LINE];
            if (!fgets(buf, sizeof(buf), stdin)) break;
            /* Remove trailing newline */
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
            line = strdup(buf);
            if (!line) break;
        }

        /* Handle line continuation (trailing hyphen) */
        while (1) {
            size_t len = strlen(line);
            /* Check for trailing - (possibly with trailing whitespace) */
            size_t end = len;
            while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t'))
                end--;
            if (end > 0 && line[end - 1] == '-') {
                line[end - 1] = '\0';

                char *cont = NULL;
                if (dcl_ctx.interactive) {
#ifdef HAVE_READLINE
                    cont = readline("_$ ");
#else
                    static char cbuf[DCL_MAX_LINE];
                    printf("_$ ");
                    fflush(stdout);
                    if (fgets(cbuf, sizeof(cbuf), stdin)) {
                        size_t clen = strlen(cbuf);
                        if (clen > 0 && cbuf[clen - 1] == '\n')
                            cbuf[clen - 1] = '\0';
                        cont = strdup(cbuf);
                    }
#endif
                } else {
                    static char cbuf[DCL_MAX_LINE];
                    if (fgets(cbuf, sizeof(cbuf), stdin)) {
                        size_t clen = strlen(cbuf);
                        if (clen > 0 && cbuf[clen - 1] == '\n')
                            cbuf[clen - 1] = '\0';
                        cont = strdup(cbuf);
                    }
                }

                if (cont) {
                    /* Strip leading $ from continuation */
                    char *cp = cont;
                    while (*cp == ' ' || *cp == '\t') cp++;
                    if (*cp == '$') {
                        cp++;
                        while (*cp == ' ' || *cp == '\t') cp++;
                    }

                    size_t curlen = strlen(line);
                    size_t contlen = strlen(cp);
                    char *combined = malloc(curlen + contlen + 2);
                    if (combined) {
                        memcpy(combined, line, curlen);
                        combined[curlen] = ' ';
                        memcpy(combined + curlen + 1, cp, contlen + 1);
                        free(line);
                        line = combined;
                    } else {
                        /* malloc failed: restore the trailing dash that was
                         * removed above so the line is not silently truncated */
                        line[end - 1] = '-';
                        free(cont);
                        break;
                    }
                    free(cont);
                } else {
                    break;
                }
            } else {
                break;
            }
        }

        process_line(&dcl_ctx, line);
        free(line);
    }

    /* Drain and tear down any mailbox-bound SYS$OUTPUT (vms-786) BEFORE the
     * rest of cleanup: this flushes DCL's final output to the parent's mailbox
     * and joins the writer thread, so the parent's last read is not lost to
     * process exit. A no-op when SYS$INPUT/SYS$OUTPUT were not mailboxes. */
    dcl_mbx_shutdown();

    /* Cleanup */
    restore_termios();
    /* Deallocate terminal device */
    vms_term_deallocate(dcl_ctx.terminal.device_name);
    /* Close any open channels */
    for (int i = 0; i < 16; i++) {
        if (dcl_ctx.channels[i].fp) {
            fclose(dcl_ctx.channels[i].fp);
            dcl_ctx.channels[i].fp = NULL;
        }
    }

    dcl_sym_cleanup();
    vms_pcb_cleanup();

    return dcl_ctx.exit_status;
}
