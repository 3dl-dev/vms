/*
 * vmssshd.c - VMS-native SSH server daemon for OVMX
 *
 * Fork-per-connection SSH server using libssh (server-side API).
 * Authenticates against SYSUAF, allocates a PTY, performs full VMS
 * LOGINOUT-equivalent session initialization, and execs vmsdcl --login.
 *
 * Usage:
 *   vmssshd [-p port] [-k host_key_path] [-d]
 *
 * %VMSSSH-I-STARTUP, server started on port 22
 *
 * Session flow: auth -> PCB init -> VMS env -> banner -> exec vmsdcl --login
 * (vms-825.3: LOGINOUT-equivalent process creation)
 */

/* _GNU_SOURCE (set globally by CMake) already implies _POSIX_C_SOURCE and _DEFAULT_SOURCE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <pwd.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/ioctl.h>

#include <pty.h>   /* openpty(), forkpty() */

#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

#include "sysuaf.h"
#include "str_util.h"
#include "term_map.h"
#include "vms/pcb.h"
#include "vms/privs.h"
#include "vms/logical.h"
#include "ovmx_accounting.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "dcl/terminal.h"

#include "ovmx_layout.h"
#include "ovmx_identity.h"
#include "ovmx_banner.h"
#define DCL_SHELL_PATH VMS_SYSTEM_DIR "/DCL.EXE"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define DEFAULT_PORT         22
#define DEFAULT_HOST_KEY     VMS_SSH_HOST_KEY
#define AUTH_TIMEOUT_SEC     30
#define MAX_AUTH_ATTEMPTS    3
#define FORWARD_BUF_SIZE     4096

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static int g_port         = DEFAULT_PORT;
static char g_host_key[256] = DEFAULT_HOST_KEY;
static int g_daemon_mode  = 0;

/* ------------------------------------------------------------------ */
/* SIGCHLD handler — reap zombie children                             */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* str_upcase() now provided by str_util.h */

/* ------------------------------------------------------------------ */
/* Window change callback context and handler                        */
/* ------------------------------------------------------------------ */

struct pty_context {
    int master_fd;
    pid_t shell_pid;
};

static int channel_pty_window_change(ssh_session session,
                                     ssh_channel channel,
                                     int cols, int rows,
                                     int px_width, int px_height,
                                     void *userdata)
{
    (void)session;
    (void)channel;
    (void)px_width;
    (void)px_height;

    struct pty_context *ctx = (struct pty_context *)userdata;
    if (!ctx || ctx->master_fd < 0)
        return -1;

    /* Update PTY window size */
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)(cols > 0 ? cols : 80);
    ws.ws_row = (unsigned short)(rows > 0 ? rows : 24);
    ioctl(ctx->master_fd, TIOCSWINSZ, &ws);

    /* Send SIGWINCH to shell process group so it picks up the new size */
    if (ctx->shell_pid > 0)
        kill(-ctx->shell_pid, SIGWINCH);

    fprintf(stderr, "%%VMSSSH-I-WINCHG, window changed to %dx%d\n", cols, rows);
    return 0;
}

/* map_term_to_vms_device_type: SSH TERM -> OVMX device-type label.
 * Moved to term_map.c/term_map.h (vms-97d) so the mapping is unit-tested
 * against the real product function instead of a copy. See term_map.h
 * for the Rule 8/Rule 10 label: this is an OVMX design choice, not
 * VMS-authentic behavior. */

/* ------------------------------------------------------------------ */
/* generate_host_key: auto-generate RSA key if missing               */
/* ------------------------------------------------------------------ */

static int generate_host_key(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return 0; /* already exists */

    /* Ensure directory exists */
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir)-1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755); /* ignore error if it exists */
    }

    fprintf(stderr, "%%VMSSSH-I-KEYGEN, generating RSA host key at %s\n", path);

    ssh_key key = NULL;
    int rc = ssh_pki_generate(SSH_KEYTYPE_RSA, 2048, &key);
    if (rc != SSH_OK) {
        fprintf(stderr, "%%VMSSSH-E-KEYFAIL, ssh_pki_generate failed: %d\n", rc);
        return -1;
    }

    rc = ssh_pki_export_privkey_file(key, NULL, NULL, NULL, path);
    ssh_key_free(key);

    if (rc != SSH_OK) {
        fprintf(stderr, "%%VMSSSH-E-KEYWRITE, cannot write host key to %s: rc=%d\n",
                path, rc);
        return -1;
    }

    chmod(path, 0600);
    fprintf(stderr, "%%VMSSSH-I-KEYOK, host key written to %s\n", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* handle_connection: child process — auth + PTY + data forwarding   */
/* ------------------------------------------------------------------ */

static void handle_connection(ssh_session session)
{
    /* Key exchange */
    if (ssh_handle_key_exchange(session) != SSH_OK) {
        fprintf(stderr, "%%VMSSSH-E-KEXFAIL, key exchange failed: %s\n",
                ssh_get_error(session));
        ssh_disconnect(session);
        ssh_free(session);
        exit(1);
    }

    /* Set auth timeout */
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &(int){AUTH_TIMEOUT_SEC});

    /* ---- Authentication phase ---- */
    int authenticated = 0;
    int auth_attempts = 0;
    char authed_user[64] = {0};

    while (!authenticated) {
        ssh_message msg = ssh_message_get(session);
        if (msg == NULL)
            break;

        int msg_type    = ssh_message_type(msg);
        int msg_subtype = ssh_message_subtype(msg);

        if (msg_type == SSH_REQUEST_AUTH) {
            if (msg_subtype == SSH_AUTH_METHOD_PASSWORD) {
                const char *user = ssh_message_auth_user(msg);
                /* ssh_message_auth_password() is deprecated in favour of the
                 * callback-based API, but we use the message-loop pattern here.
                 * Suppress the deprecation warning for this specific call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                const char *pass = ssh_message_auth_password(msg);
#pragma GCC diagnostic pop

                if (!user) {
                    ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(msg);
                    ssh_message_free(msg);
                    continue;
                }

                /* Copy and upcase username for SYSUAF (VMS usernames are uppercase) */
                char uname[64];
                strncpy(uname, user, sizeof(uname) - 1);
                uname[sizeof(uname)-1] = '\0';
                str_upcase(uname);

                sysuaf_record_t rec;
                int found = (sysuaf_lookup(uname, &rec) == 0);

                if (found && sysuaf_authenticate(&rec, pass ? pass : "")) {
                    strncpy(authed_user, uname, sizeof(authed_user) - 1);
                    ssh_message_auth_reply_success(msg, 0);
                    authenticated = 1;
                    fprintf(stderr, "%%VMSSSH-I-LOGIN, user %s authenticated\n", uname);
                } else {
                    auth_attempts++;
                    fprintf(stderr, "%%VMSSSH-W-AUTHERR, authentication failed for user %s"
                            " (attempt %d)\n", uname, auth_attempts);
                    if (auth_attempts >= MAX_AUTH_ATTEMPTS) {
                        ssh_message_reply_default(msg);
                        ssh_message_free(msg);
                        break;
                    }
                    ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_PASSWORD);
                    ssh_message_reply_default(msg);
                }
            } else {
                /* Only password auth supported */
                ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_PASSWORD);
                ssh_message_reply_default(msg);
            }
        } else {
            ssh_message_reply_default(msg);
        }

        ssh_message_free(msg);
    }

    if (!authenticated) {
        fprintf(stderr, "%%VMSSSH-W-AUTHFAIL, authentication failed — closing\n");
        ssh_disconnect(session);
        ssh_free(session);
        exit(1);
    }

    /* ---- Channel phase ---- */
    ssh_channel channel = NULL;
    int got_shell = 0;

    /* PTY dimensions and terminal type (set by pty-req) */
    int pty_cols = 80;
    int pty_rows = 24;
    char ssh_term[64] = {0};  /* TERM value from SSH client */

    while (!got_shell) {
        ssh_message msg = ssh_message_get(session);
        if (msg == NULL)
            break;

        int msg_type    = ssh_message_type(msg);
        int msg_subtype = ssh_message_subtype(msg);

        if (msg_type == SSH_REQUEST_CHANNEL_OPEN &&
            msg_subtype == SSH_CHANNEL_SESSION) {
            channel = ssh_message_channel_request_open_reply_accept(msg);
            if (!channel) {
                fprintf(stderr, "%%VMSSSH-E-CHANFAIL, cannot open channel\n");
                ssh_message_free(msg);
                break;
            }
        } else if (msg_type == SSH_REQUEST_CHANNEL) {
            if (msg_subtype == SSH_CHANNEL_REQUEST_PTY) {
                /* Extract TERM and dimensions — these accessors are deprecated
                 * in favour of the callback API, but required for the
                 * message-loop pattern. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                const char *req_term = ssh_message_channel_request_pty_term(msg);
                if (req_term && req_term[0]) {
                    strncpy(ssh_term, req_term, sizeof(ssh_term) - 1);
                    ssh_term[sizeof(ssh_term) - 1] = '\0';
                }
                pty_cols = ssh_message_channel_request_pty_width(msg);
                pty_rows = ssh_message_channel_request_pty_height(msg);
#pragma GCC diagnostic pop
                if (pty_cols <= 0) pty_cols = 80;
                if (pty_rows <= 0) pty_rows = 24;
                ssh_message_channel_request_reply_success(msg);
            } else if (msg_subtype == SSH_CHANNEL_REQUEST_SHELL ||
                       msg_subtype == SSH_CHANNEL_REQUEST_EXEC) {
                ssh_message_channel_request_reply_success(msg);
                got_shell = 1;
                ssh_message_free(msg);
                break;
            } else {
                ssh_message_reply_default(msg);
            }
        } else {
            ssh_message_reply_default(msg);
        }

        ssh_message_free(msg);
    }

    if (!channel || !got_shell) {
        fprintf(stderr, "%%VMSSSH-W-NOCHAN, no channel/shell established\n");
        ssh_disconnect(session);
        ssh_free(session);
        exit(1);
    }

    /* ---- Allocate PTY and fork shell ---- */
    int master_fd = -1;
    int slave_fd  = -1;
    char slave_name[256];

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)pty_cols;
    ws.ws_row = (unsigned short)pty_rows;

    if (openpty(&master_fd, &slave_fd, slave_name, NULL, &ws) < 0) {
        fprintf(stderr, "%%VMSSSH-E-PTYOPEN, openpty() failed: %s\n", strerror(errno));
        ssh_channel_send_eof(channel);
        ssh_disconnect(session);
        ssh_free(session);
        exit(1);
    }

    /* Look up SYSUAF record for session setup */
    sysuaf_record_t sysuaf_rec;
    memset(&sysuaf_rec, 0, sizeof(sysuaf_rec));
    if (sysuaf_lookup(authed_user, &sysuaf_rec) != 0) {
        /* Should not happen — user was authenticated above — but be safe */
        fprintf(stderr, "%%VMSSSH-E-UAFMISS, cannot re-read SYSUAF for %s\n", authed_user);
        strncpy(sysuaf_rec.username, authed_user, sizeof(sysuaf_rec.username) - 1);
        strncpy(sysuaf_rec.default_dir, "/", sizeof(sysuaf_rec.default_dir) - 1);
    }

    const char *home_dir = (sysuaf_rec.default_dir[0] != '\0')
                           ? sysuaf_rec.default_dir : "/";

    pid_t shell_pid = fork();
    if (shell_pid < 0) {
        fprintf(stderr, "%%VMSSSH-E-FORKFAIL, fork() failed: %s\n", strerror(errno));
        close(master_fd);
        close(slave_fd);
        ssh_channel_send_eof(channel);
        ssh_disconnect(session);
        ssh_free(session);
        exit(1);
    }

    if (shell_pid == 0) {
        /* ---- Child: set up PTY slave and exec VMS session (LOGINOUT-equivalent) ---- */
        close(master_fd);

        /* New session, PTY slave becomes controlling terminal */
        if (setsid() < 0) {
            perror("setsid");
            exit(1);
        }

        /* Make slave the controlling terminal */
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            perror("TIOCSCTTY");
            /* non-fatal on some kernels */
        }

        /* Redirect stdio to PTY slave */
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);

        if (slave_fd > STDERR_FILENO)
            close(slave_fd);

        /* ---- Step 1: Initialize PCB with user identity ---- */
        uint64_t user_privs = parse_privilege_string(sysuaf_rec.privileges);
        struct vms_pcb *pcb = vms_pcb_init(user_privs);

        /*
         * DELETED (vms-fb9): this took a name out of the private "_FTA"
         * pool file -- falling back to the literal "_FTA0:" when even that
         * failed -- and used it as the session's PROCESS name. A terminal
         * device name is not a process name, the pool is not the executive,
         * and the fallback was an invented VMS device name. The process
         * name is executive-owned state OVMX does not have yet, so the
         * session is created without one rather than with a fake one.
         */
        if (pcb) {
            uint32_t uic = (sysuaf_rec.uic_group << 16) | sysuaf_rec.uic_member;
            vms_pcb_set_identity((uint32_t)getpid(), uic, sysuaf_rec.username, "");
            vms_pcb_set_default_dir(sysuaf_rec.default_dir);
        }

        /* ---- Step 2: Set up VMS environment variables ---- */
        /* Use SSH client's TERM value if provided, else default to vt100 */
        setenv("TERM", ssh_term[0] ? ssh_term : "vt100", 1);

        /*
         * DELETED, NOT REPLACED (vms-fb9): setenv("VMS_DEVICE_TYPE", ...)
         * and setenv("VMS_TERMINAL", term_dev, 1) stood here, handing a
         * terminal identity down to the session through the environment.
         * That is the rejected VMS_PRCNAM shape (CLAUDE.md rule 10,
         * worked example 2) -- the session ended up telling itself what
         * terminal it was on, and nothing else on the node could see or
         * contradict it.
         *
         * A VMS terminal is a device in the executive's device table
         * ($ASSIGN to it, $GETDVI on the channel). A REMOTE terminal is
         * not in that table at all: the executive creates only the
         * console OPA0: today, and how a network session's terminal gets
         * created is not something this file may decide on its own. So
         * nothing is passed, and the SSH session simply has no VMS
         * terminal name until the executive can give it one.
         *
         * SIDE EFFECT, recorded so it is not discovered as a surprise:
         * vmsssh_map_term_to_device_type() (src/vmsssh/vmsssh_term.c) has
         * no production caller left. tests/vmsssh/test_term_mapping.c
         * still exercises it, so the test now covers a function nothing
         * calls. Neither the function nor the test is deleted here --
         * deleting coverage is not this item's call.
         */
        setenv("HOME",        home_dir,               1);
        setenv("USER",        authed_user,            1);
        setenv("LOGNAME",     authed_user,            1);
        setenv("SHELL",       DCL_SHELL_PATH,         1);

        setenv("VMS_USERNAME",    sysuaf_rec.username,    1);

        char uic_group_str[16], uic_member_str[16];
        snprintf(uic_group_str,  sizeof(uic_group_str),  "%u", sysuaf_rec.uic_group);
        snprintf(uic_member_str, sizeof(uic_member_str), "%u", sysuaf_rec.uic_member);
        setenv("VMS_UIC_GROUP",   uic_group_str,  1);
        setenv("VMS_UIC_MEMBER",  uic_member_str, 1);
        setenv("VMS_DEFAULT_DIR", sysuaf_rec.default_dir, 1);
        setenv("VMS_PRIVILEGES",  sysuaf_rec.privileges,  1);
        setenv("SYS$LOGIN",       sysuaf_rec.default_dir, 1);
        setenv("SYS$SCRATCH",     "/tmp",                 1);

        /* ---- Step 3: Display VMS login banner ---- */
        static const char *months[] = {
            "JAN","FEB","MAR","APR","MAY","JUN",
            "JUL","AUG","SEP","OCT","NOV","DEC"
        };

        /* SYS$WELCOME (boot-defined logical), falling back to the built-in
         * badged OVMX identity. The old hardcoded "Welcome to OpenVMS (tm)
         * OVMX V7.3" both ignored the logical and claimed VSI's mark. */
        printf("\n");
        ovmx_banner_welcome(stdout);

        time_t last_login = 0;
        if (ovmx_accounting_get_lastlogin(sysuaf_rec.username, &last_login) == 0
                && last_login > 0) {
            struct tm *tm = localtime(&last_login);
            if (tm) {
                printf("\n   Last interactive login on %02d-%s-%04d %02d:%02d:%02d\n\n",
                       tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900,
                       tm->tm_hour, tm->tm_min, tm->tm_sec);
            } else {
                printf("\n   Last login time could not be determined.\n\n");
            }
        } else {
            printf("\n   No previous interactive login recorded.\n\n");
        }

        /* Record this login (after displaying last, before launching DCL) */
        ovmx_accounting_record_login(sysuaf_rec.username);
        fflush(stdout);

        /* ---- Step 4: Exec vmsdcl --login ---- */
        execl(DCL_SHELL_PATH, "vmsdcl", "--login", (char *)NULL);

        /* Fallback if vmsdcl is not installed */
        perror("vmsdcl");
        fprintf(stderr, "%%VMSSSH-W-NOSHELL, vmsdcl not found — falling back to /bin/sh\n");
        execl("/bin/sh", "sh", (char *)NULL);
        _exit(1);
    }

    /* ---- Parent: forward data between SSH channel and PTY master ---- */
    close(slave_fd);

    /* Set up channel callback for window-change requests */
    struct pty_context pty_ctx = { .master_fd = master_fd, .shell_pid = shell_pid };
    struct ssh_channel_callbacks_struct channel_cb;
    memset(&channel_cb, 0, sizeof(channel_cb));
    ssh_callbacks_init(&channel_cb);
    channel_cb.userdata = &pty_ctx;
    channel_cb.channel_pty_window_change_function = channel_pty_window_change;
    ssh_set_channel_callbacks(channel, &channel_cb);

    /* Set master fd non-blocking */
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    char buf[FORWARD_BUF_SIZE];
    int client_eof = 0;   /* client half-closed its input; keep draining the PTY */

    for (;;) {
        struct pollfd fds[2];

        /* End the session only when the channel is fully CLOSED — not on
         * channel EOF alone. A scripted/piped client (e.g. an ssh heredoc)
         * closes its stdin right after sending input, which raises
         * channel-EOF while the DCL session is still starting up and
         * producing output. Ending here tore the session down before any
         * output (even the login banner) reached the client (vms-83e).
         * Instead we treat client-EOF as a half-close: stop reading input
         * but keep forwarding PTY->channel until the shell child exits
         * (it will, once it processes the piped input — e.g. LOGOUT).
         * A real interactive user never EOFs stdin, so their sessions are
         * unaffected and still end via PTY hangup / child exit below. */
        if (ssh_channel_is_closed(channel))
            break;

        /* Poll: SSH channel fd (via session socket) + PTY master */
        int ssh_fd = ssh_get_fd(session);

        /* Once the client has half-closed, stop polling the channel for
         * input (fd = -1 is ignored by poll) — otherwise poll would spin
         * on the readable-but-EOF descriptor. */
        fds[0].fd      = client_eof ? -1 : ssh_fd;
        fds[0].events  = POLLIN;
        fds[0].revents = 0;

        fds[1].fd      = master_fd;
        fds[1].events  = POLLIN;
        fds[1].revents = 0;

        int ret = poll(fds, 2, 200); /* short timeout: also drains libssh-buffered
                                      * channel data the socket won't re-signal */

        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Data from SSH channel -> PTY master.
         *
         * Do NOT gate this read on the socket's POLLIN. libssh reads the
         * client's channel payload off the socket into its own per-channel
         * buffer during packet processing, so once a scripted client (an ssh
         * heredoc) has sent its commands, the socket has nothing more to
         * signal (no POLLIN) while those commands sit unread inside libssh.
         * Polling the socket alone then never drains them: the session reaches
         * the DCL prompt but the piped commands never drive it, and when EOF is
         * observed we stop before draining — the flaky-UAT failure (vms-e18).
         * ssh_channel_read_nonblocking() both pumps any pending socket packets
         * AND returns already-buffered bytes, so we call it every iteration
         * (poll's short timeout just throttles the idle spin). */
        if (!client_eof) {
            int n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0);
            if (n > 0) {
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(master_fd, buf + written, (size_t)(n - written));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        goto done;
                    }
                    written += w;
                }
            } else if (n == SSH_EOF || (n == 0 && ssh_channel_is_eof(channel))) {
                /* Client half-closed its input. A scripted client (an ssh
                 * heredoc) EOFs its stdin right after sending commands, and
                 * libssh signals this as SSH_EOF (-127) from
                 * ssh_channel_read_nonblocking() — it is NOT an error. The old
                 * code lumped -127 into "n < 0" and broke the loop, tearing the
                 * whole session down on the client's normal EOF and SIGHUP-ing
                 * DCL before it could read the piped commands off the PTY (the
                 * flaky-UAT root cause, vms-e18). Treat it as a half-close:
                 * stop reading input but keep the PTY alive and keep forwarding
                 * DCL output until the shell child exits (it will, once it runs
                 * the piped commands through LOGOUT). A real interactive user
                 * never EOFs stdin, so their sessions are unaffected. */
                client_eof = 1;
            } else if (n < 0) {
                break;                     /* genuine channel error (SSH_ERROR) */
            }
        }

        /* Data from PTY master -> SSH channel */
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                /* Write the whole buffer. ssh_channel_write() may write fewer
                 * bytes than requested; ignoring its return silently drops
                 * session output (an intermittent missing line — residual
                 * vms-e18). Loop until all n bytes are sent. */
                ssize_t off = 0;
                while (off < n) {
                    int w = ssh_channel_write(channel, buf + off,
                                              (uint32_t)(n - off));
                    if (w == SSH_ERROR)
                        goto done;
                    off += w;
                }
            } else if (n < 0) {
                if (errno == EIO)
                    break; /* PTY slave closed: shell exited */
                /* EAGAIN / other: no data available, continue */
            }
        }

        /* PTY hangup: shell exited */
        if (fds[1].revents & (POLLHUP | POLLERR))
            break;

        /* Check if shell child exited */
        if (waitpid(shell_pid, NULL, WNOHANG) == shell_pid) {
            shell_pid = -1;  /* Already reaped */
            break;
        }
    }

done:
    /* Final drain: DCL may have written its last output into the PTY just
     * before exiting, and the forward loop breaks on child-exit/hangup without
     * necessarily having read it. Flush anything still readable to the channel
     * before we close, so the tail of the session is not lost (residual
     * vms-e18). Bounded by poll's 200ms idle timeout. */
    if (master_fd >= 0 && channel && !ssh_channel_is_closed(channel)) {
        for (;;) {
            struct pollfd pfd = { .fd = master_fd, .events = POLLIN, .revents = 0 };
            if (poll(&pfd, 1, 200) <= 0)
                break;
            if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
                break;
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            ssize_t off = 0;
            while (off < n) {
                int w = ssh_channel_write(channel, buf + off, (uint32_t)(n - off));
                if (w == SSH_ERROR) { off = n; break; }
                off += w;
            }
        }
    }

    /* Clean up */
    close(master_fd);
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);

    /* Wait for shell to exit (if not already reaped) */
    if (shell_pid > 0)
        waitpid(shell_pid, NULL, 0);

    ssh_disconnect(session);
    ssh_free(session);

    fprintf(stderr, "%%VMSSSH-I-LOGOUT, session ended for user %s\n", authed_user);
    exit(0);
}

/* ------------------------------------------------------------------ */
/* parse_args                                                          */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-p port] [-k host_key_path] [-d]\n"
            "  -p port          Listen port (default: %d)\n"
            "  -k path          Host key file (default: %s)\n"
            "  -d               Daemon mode (fork to background)\n",
            prog, DEFAULT_PORT, DEFAULT_HOST_KEY);
}

static int parse_args(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "p:k:dh")) != -1) {
        switch (opt) {
        case 'p':
            g_port = atoi(optarg);
            if (g_port <= 0 || g_port > 65535) {
                fprintf(stderr, "%%VMSSSH-E-BADPORT, invalid port: %s\n", optarg);
                return -1;
            }
            break;
        case 'k':
            strncpy(g_host_key, optarg, sizeof(g_host_key) - 1);
            g_host_key[sizeof(g_host_key)-1] = '\0';
            break;
        case 'd':
            g_daemon_mode = 1;
            break;
        case 'h':
            usage(argv[0]);
            return -1;
        default:
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    /* Bootstrap VMS namespace */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    if (parse_args(argc, argv) < 0)
        return 1;

    /* Translate VMS filespec to Linux path if g_host_key looks like a VMS spec */
    if (strchr(g_host_key, ':') && !strchr(g_host_key, '/')) {
        char host_key_linux[256];
        vmsfs_to_linux_path(g_host_key, host_key_linux, sizeof(host_key_linux));
        strncpy(g_host_key, host_key_linux, sizeof(g_host_key) - 1);
        g_host_key[sizeof(g_host_key) - 1] = '\0';
    }

    /* Auto-generate host key if missing */
    if (generate_host_key(g_host_key) < 0)
        return 1;

    /* Daemonize if requested */
    if (g_daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid > 0)
            return 0; /* parent exits */
        setsid();
        /* redirect stdio to /dev/null */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            /* keep stderr for logging */
            close(devnull);
        }
    }

    /* SIGCHLD: reap children */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    /* Ignore SIGPIPE — broken SSH connections should not kill the server */
    signal(SIGPIPE, SIG_IGN);

    /* Create ssh_bind */
    ssh_bind sshbind = ssh_bind_new();
    if (!sshbind) {
        fprintf(stderr, "%%VMSSSH-F-BIND, ssh_bind_new() failed\n");
        return 1;
    }

    /* Configure bind */
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT, &g_port);
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_RSAKEY, g_host_key);

    /* Start listening */
    if (ssh_bind_listen(sshbind) < 0) {
        fprintf(stderr, "%%VMSSSH-F-LISTEN, cannot listen on port %d: %s\n",
                g_port, ssh_get_error(sshbind));
        ssh_bind_free(sshbind);
        return 1;
    }

    fprintf(stderr, "%%VMSSSH-I-STARTUP, vmssshd listening on port %d\n", g_port);

    /* Accept loop */
    for (;;) {
        ssh_session session = ssh_new();
        if (!session) {
            fprintf(stderr, "%%VMSSSH-E-NOMEM, ssh_new() failed\n");
            sleep(1);
            continue;
        }

        int rc = ssh_bind_accept(sshbind, session);
        if (rc != SSH_OK) {
            fprintf(stderr, "%%VMSSSH-E-ACCEPT, ssh_bind_accept failed: %s\n",
                    ssh_get_error(sshbind));
            ssh_free(session);
            continue;
        }

        fprintf(stderr, "%%VMSSSH-I-CONNECT, new connection accepted\n");

        pid_t child = fork();
        if (child < 0) {
            fprintf(stderr, "%%VMSSSH-E-FORKFAIL, fork() failed: %s\n", strerror(errno));
            ssh_free(session);
            continue;
        }

        if (child == 0) {
            /* Child: close the bind socket and handle the connection */
            ssh_bind_free(sshbind);
            handle_connection(session);
            /* handle_connection calls exit(), but just in case: */
            exit(0);
        }

        /* Parent: free our reference to the session (child owns it) */
        ssh_free(session);
    }

    ssh_bind_free(sshbind);
    return 0;
}
