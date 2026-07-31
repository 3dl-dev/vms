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
#include <pwd.h>
#include <sys/utsname.h>
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
#include "ssdef.h"
#include "vms/pcb.h"
#include "ovmx_identity.h"
#include "vms/privs.h"
#include "vms/logical.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"

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

    /* Get user info */
    struct passwd *pw = getpwuid(getuid());
    if (pw) {
        size_t i;
        for (i = 0; i < sizeof(ctx->username) - 1 && pw->pw_name[i]; i++) {
            ctx->username[i] = (char)toupper((unsigned char)pw->pw_name[i]);
        }
        ctx->username[i] = '\0';
    } else {
        strcpy(ctx->username, "SYSTEM");
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
     */

    /*
     * ============================================================
     * STOPGAP -- PRIVILEGE ESCALATION PATH, STILL OPEN (vms-2b8)
     * ============================================================
     * The reads below take this process's USERNAME and PRIVILEGE MASK
     * from ordinary environment variables. Any process can setenv()
     * them, so any process can choose its own identity here. This is
     * the env-var facade CLAUDE.md Rule 10 names as a worked example,
     * and it is NOT an access control system.
     *
     * The executive now owns identity for real: it derives the UIC and
     * the authorized privilege mask from the task's credentials and
     * refuses to let a process widen either (src/kernel/vms_proctab.c
     * vms_ioctl_setident, proven by tests/qemu/test_kmod_ident.c
     * against a real /dev/vms). The correct code here is a
     * vms_kif_getjpi_self() read of that row.
     *
     * STILL NOT WIRED UP -- BUT THE REASON PREVIOUSLY WRITTEN HERE IS
     * NOW FALSE, RE-CHECKED 2026-07-31 (vms-fb9 r6) BY EXECUTION. The
     * literal premise survives: vms_kif_register() still has zero
     * product callers by that name. The CONCLUSION drawn from it --
     * "no DCL process is registered with the executive and $GETJPI
     * would return -ESRCH for every one of them" -- does not, because
     * vms-9fc made the open -> register sequence automatic
     * (src/libvmssys/vms_kif.h:7-18): kif_bind() in
     * src/libvmssys/vms_kif.c completes it before every /dev/vms ioctl
     * a process issues through vms_kif.c's public entry points, keyed
     * on that process, so a DCL process is registered on its FIRST
     * vms_kif_* call with no explicit register call anywhere in its
     * path. SHOW DEVICE proves this today (src/vmsdcl/dcl_cmd_show.c,
     * vms-fb9): it reads a real /dev/vms inside QEMU with no
     * vms_kif_register() call written anywhere near it.
     *
     * "EVERY /dev/vms ioctl" was NOT true without exception until r6 of
     * this item -- r5's version of this comment said it was, and that
     * was CAUGHT BY MEASUREMENT: vms_kif_setident() issued a raw ioctl
     * instead of going through kif_call()/KIF_CALL, so it alone reached
     * the executive unbound. DCL does not call vms_kif_setident() on
     * this path (it is out of scope here, per the note below), so the
     * gap did not change what THIS file observes -- but the comment's
     * "EVERY" claim about the mechanism was false regardless of who
     * calls it, and is fixed at the source in r6
     * (src/libvmssys/vms_kif.c, tests/qemu/test_kmod_bind.c suite 0),
     * not by narrowing this sentence.
     *
     * So the precondition this note told the next reader to wait for
     * has been met. That does NOT mean this file should wire up
     * vms_kif_getjpi_self() here -- replacing the identity/privilege
     * stopgap is vms-2b8's call, is behind an operator ruling, and is
     * out of scope for vms-fb9 (which owns the device-table readers,
     * not process identity). DO NOT "IMPROVE" THIS PATH under this
     * item. What is withdrawn is only the instruction to keep waiting
     * "once vms-9fc lands" -- it has landed; the stopgap's disposition
     * from here belongs to vms-2b8. The UIC half of the same defect was
     * already deleted from src/vmsrms/rms_core.c under this item,
     * because there the real credentials were available locally; the
     * user name and the privilege mask still have no local honest
     * source, which is exactly vms-2b8's territory to close.
     * ============================================================
     */
    const char *env_user = getenv("VMS_USERNAME");
    if (env_user && env_user[0]) {
        strncpy(ctx->username, env_user, sizeof(ctx->username) - 1);
        ctx->username[sizeof(ctx->username) - 1] = '\0';
    }

    const char *env_group = getenv("VMS_UIC_GROUP");
    if (env_group) ctx->uic_group = (uint32_t)strtoul(env_group, NULL, 10);

    const char *env_member = getenv("VMS_UIC_MEMBER");
    if (env_member) ctx->uic_member = (uint32_t)strtoul(env_member, NULL, 10);

    const char *env_privs = getenv("VMS_PRIVILEGES");
    if (env_privs) ctx->privileges = parse_privilege_string(env_privs);

    const char *env_defdir = getenv("VMS_DEFAULT_DIR");
    if (env_defdir && env_defdir[0]) {
        strncpy(ctx->default_dir, env_defdir, sizeof(ctx->default_dir) - 1);
        ctx->default_dir[sizeof(ctx->default_dir) - 1] = '\0';
    }

    /* Default protection: S:RWED,O:RWED,G:RE,W: = 0xFF00 */
    ctx->default_protection = 0xFF00;

    /* Initialize PCB from environment if not already done */
    if (!vms_pcb_get()) {
        uint64_t privs = env_privs ? parse_privilege_string(env_privs) : 0;

        struct vms_pcb *pcb = vms_pcb_init(privs);
        if (pcb) {
            uint32_t uic = 0;
            if (env_group && env_member)
                uic = ((uint32_t)strtoul(env_group, NULL, 10) << 16) |
                       (uint32_t)strtoul(env_member, NULL, 10);

            vms_pcb_set_identity((uint32_t)getpid(), uic,
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
 * Display the VMS-style login banner.
 */
static void display_banner(void)
{
    struct utsname uts;
    uname(&uts);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char sysname[OVMX_IDENTITY_MAXLEN];
    ovmx_node_name(sysname, sizeof(sysname));

    printf("\n");
    printf("        %s  on node %-8s %2d-%s-%04d %02d:%02d:%02d.%02d\n",
           ovmx_product_banner(), sysname, tm.tm_mday, vms_months[tm.tm_mon],
           1900 + tm.tm_year, tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 10000000));
    printf("\n");

    /* Last login message — read from per-user lastlogin file if available */
    const char *vms_user = getenv("VMS_USERNAME");
    if (vms_user && vms_user[0]) {
        char lastlogin_dir_linux[1024];
        vmsfs_to_linux_path(VMS_LASTLOGIN_DIR, lastlogin_dir_linux, sizeof(lastlogin_dir_linux));
        char lastlogin_path[512];
        snprintf(lastlogin_path, sizeof(lastlogin_path),
                 "%s/%s", lastlogin_dir_linux, vms_user);
        FILE *lf = fopen(lastlogin_path, "r");
        if (lf) {
            char prev[64];
            if (fgets(prev, sizeof(prev), lf) != NULL) {
                /* trim trailing newline */
                size_t plen = strlen(prev);
                while (plen > 0 && (prev[plen-1] == '\n' || prev[plen-1] == '\r'))
                    prev[--plen] = '\0';
                if (prev[0])
                    printf("    Last interactive login on %s\n\n", prev);
            }
            fclose(lf);
        }
    }
}

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
         * Override SYS$DISK with the process default device.
         * On a VMS system this would be set from the process PCB;
         * here we default to the system device.
         */
        lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$DISK",
                   "SYS$SYSDEVICE", 0, LNM_MODE_USER);
    }

    /* Register built-in commands */
    dcl_register_builtins();

    /* Set initial special symbols */
    dcl_sym_set("$STATUS", "1", DCL_SYM_GLOBAL);
    dcl_sym_set("$SEVERITY", "1", DCL_SYM_GLOBAL);
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

    /* Execute the command */
    int status = dcl_execute_line(substituted);

    /* Update $STATUS and $SEVERITY */
    ctx->last_status = (uint32_t)status;
    ctx->last_severity = (uint32_t)(status & 7);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", status);
    dcl_sym_set("$STATUS", buf, DCL_SYM_GLOBAL);
    snprintf(buf, sizeof(buf), "%d", status & 7);
    dcl_sym_set("$SEVERITY", buf, DCL_SYM_GLOBAL);

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

    /* Set VEOF to Ctrl+Z (VMS convention) */
    setup_vms_eof();

    /* Check for --login flag */
    int login_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--login") == 0) {
            login_mode = 1;
            dcl_ctx.logged_in = 1;
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

    /* Interactive mode */
    if (dcl_ctx.interactive) {
        /* Skip banner in login mode (vms_login already printed welcome) */
        if (!login_mode) {
            display_banner();
        }

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
#endif
    }

    /* Execute login scripts when in login mode */
    if (login_mode) {
        struct stat st;
        /* System-wide login script */
        char sylogin_linux[1024];
        vmsfs_to_linux_path(SYLOGIN_PATH, sylogin_linux, sizeof(sylogin_linux));
        if (stat(sylogin_linux, &st) == 0 && S_ISREG(st.st_mode)) {
            dcl_execute_script(sylogin_linux, 0, NULL);
        }
        /* Per-user login script */
        const char *home = getenv("SYS$LOGIN");
        if (home && home[0]) {
            char user_login[512];
            snprintf(user_login, sizeof(user_login), "%s/LOGIN.COM", home);
            if (stat(user_login, &st) == 0 && S_ISREG(st.st_mode)) {
                dcl_execute_script(user_login, 0, NULL);
            }
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
