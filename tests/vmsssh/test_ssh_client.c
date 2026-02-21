/*
 * test_ssh_client.c - libssh-based SSH client for vmssshd integration tests
 *
 * Connects to vmssshd on a given host:port, authenticates with
 * username/password, optionally runs a command, captures output,
 * and exits 0 on success.
 *
 * Usage:
 *   test_ssh_client -h host -p port -u user -P password [-c command] [-e expect]
 *
 *   -h host      SSH server hostname (default: 127.0.0.1)
 *   -p port      SSH server port (default: 22)
 *   -u user      Username
 *   -P password  Password (empty string = no password)
 *   -c command   Command to execute (if omitted, requests a shell and exits)
 *   -e expect    String that must appear in command output (case-insensitive)
 *   -x           Expect authentication to FAIL (exit 0 if auth rejected)
 *   -t secs      Connect timeout in seconds (default: 10)
 *   -v           Verbose (print output to stdout)
 *
 * Exit codes:
 *   0 - test passed (auth succeeded + optional expect matched)
 *   1 - test failed
 *   2 - usage error
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#include <libssh/libssh.h>

#define MAX_OUTPUT   (64 * 1024)
#define READ_TIMEOUT 15  /* seconds to wait for command output */

static int verbose = 0;

/* Case-insensitive substring search */
static int contains_icase(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i+j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s -u user -P password [-h host] [-p port] [-c cmd] "
        "[-e expect] [-x] [-t secs] [-v]\n", prog);
    exit(2);
}

int main(int argc, char *argv[])
{
    const char *host     = "127.0.0.1";
    int         port     = 22;
    const char *username = NULL;
    const char *password = NULL;
    const char *command  = NULL;
    const char *expect   = NULL;
    int         expect_fail = 0;
    int         timeout  = 10;
    int         opt;

    while ((opt = getopt(argc, argv, "h:p:u:P:c:e:xt:v")) != -1) {
        switch (opt) {
        case 'h': host        = optarg; break;
        case 'p': port        = atoi(optarg); break;
        case 'u': username    = optarg; break;
        case 'P': password    = optarg; break;
        case 'c': command     = optarg; break;
        case 'e': expect      = optarg; break;
        case 'x': expect_fail = 1;      break;
        case 't': timeout     = atoi(optarg); break;
        case 'v': verbose     = 1;      break;
        default:  usage(argv[0]);
        }
    }

    if (!username || !password) {
        fprintf(stderr, "error: -u and -P are required\n");
        usage(argv[0]);
    }

    /* ------------------------------------------------------------------ */
    /* Create and configure session                                        */
    /* ------------------------------------------------------------------ */
    ssh_session session = ssh_new();
    if (!session) {
        fprintf(stderr, "error: ssh_new() failed\n");
        return 1;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, host);
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, username);
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    /* Disable strict host-key checking for test environment.
     * Point known_hosts at /dev/null so libssh never finds a cached key
     * and never tries to write one — it will treat every host as unknown
     * but still connect (SSH_KNOWN_HOSTS_UNKNOWN is not a fatal error
     * unless we explicitly reject it, which we don't). */
    int verbosity = SSH_LOG_NOLOG;
    if (verbose) verbosity = SSH_LOG_WARNING;
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS,       "/dev/null");
    ssh_options_set(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/dev/null");

    /* ------------------------------------------------------------------ */
    /* Connect                                                             */
    /* ------------------------------------------------------------------ */
    int rc = ssh_connect(session);
    if (rc != SSH_OK) {
        fprintf(stderr, "error: connect to %s:%d failed: %s\n",
                host, port, ssh_get_error(session));
        ssh_free(session);
        return 1;
    }

    /* Accept any host key — we pointed known_hosts at /dev/null above.
     * ssh_session_is_known_server() will return SSH_KNOWN_HOSTS_UNKNOWN,
     * which is fine for a test client; we proceed without rejecting. */
    {
#if LIBSSH_VERSION_INT >= SSH_VERSION_INT(0,9,0)
        enum ssh_known_hosts_e known = ssh_session_is_known_server(session);
        (void)known; /* SSH_KNOWN_HOSTS_UNKNOWN is expected and acceptable */
#else
        int known = ssh_is_server_known(session);
        (void)known; /* SSH_SERVER_NOT_KNOWN is expected and acceptable */
#endif
    }

    /* ------------------------------------------------------------------ */
    /* Authenticate                                                        */
    /* ------------------------------------------------------------------ */
    rc = ssh_userauth_password(session, NULL, password);

    if (expect_fail) {
        /* We expect authentication to be rejected */
        if (rc == SSH_AUTH_SUCCESS) {
            fprintf(stderr, "FAIL: expected auth failure but auth succeeded for %s\n",
                    username);
            ssh_disconnect(session);
            ssh_free(session);
            return 1;
        }
        if (verbose)
            printf("PASS: auth correctly rejected for user %s\n", username);
        ssh_disconnect(session);
        ssh_free(session);
        return 0;
    }

    if (rc != SSH_AUTH_SUCCESS) {
        fprintf(stderr, "FAIL: authentication failed for %s: %s\n",
                username, ssh_get_error(session));
        ssh_disconnect(session);
        ssh_free(session);
        return 1;
    }

    if (verbose)
        printf("auth succeeded for %s\n", username);

    /* ------------------------------------------------------------------ */
    /* Open a session channel                                              */
    /* ------------------------------------------------------------------ */
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        fprintf(stderr, "error: ssh_channel_new() failed\n");
        ssh_disconnect(session);
        ssh_free(session);
        return 1;
    }

    rc = ssh_channel_open_session(channel);
    if (rc != SSH_OK) {
        fprintf(stderr, "error: ssh_channel_open_session() failed: %s\n",
                ssh_get_error(session));
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Execute command or request shell                                    */
    /* ------------------------------------------------------------------ */
    if (command) {
        rc = ssh_channel_request_exec(channel, command);
        if (rc != SSH_OK) {
            fprintf(stderr, "error: ssh_channel_request_exec() failed: %s\n",
                    ssh_get_error(session));
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            ssh_disconnect(session);
            ssh_free(session);
            return 1;
        }
    } else {
        /* Request a PTY then a shell (so vmssshd sees SSH_CHANNEL_REQUEST_SHELL) */
        rc = ssh_channel_request_pty(channel);
        if (rc != SSH_OK && verbose)
            fprintf(stderr, "note: pty request failed (may be OK): %s\n",
                    ssh_get_error(session));

        rc = ssh_channel_request_shell(channel);
        if (rc != SSH_OK) {
            fprintf(stderr, "error: ssh_channel_request_shell() failed: %s\n",
                    ssh_get_error(session));
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            ssh_disconnect(session);
            ssh_free(session);
            return 1;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Read output                                                         */
    /* ------------------------------------------------------------------ */
    char  *output  = malloc(MAX_OUTPUT + 1);
    size_t out_len = 0;

    if (!output) {
        fprintf(stderr, "error: malloc failed\n");
        return 1;
    }
    output[0] = '\0';

    /* Read until EOF or timeout */
    int eof_seen = 0;
    time_t deadline = time(NULL) + READ_TIMEOUT;

    while (!eof_seen && time(NULL) < deadline) {
        /* Poll for data */
        rc = ssh_channel_poll_timeout(channel, 500 /* ms */, 0 /* is_stderr */);
        if (rc == SSH_ERROR)
            break;
        if (rc == SSH_EOF) {
            eof_seen = 1;
            break;
        }
        if (rc > 0) {
            char buf[4096];
            int n = ssh_channel_read_timeout(channel, buf, sizeof(buf) - 1,
                                             0 /* stderr */, 1000 /* ms */);
            if (n > 0) {
                if (out_len + (size_t)n < MAX_OUTPUT) {
                    memcpy(output + out_len, buf, n);
                    out_len += n;
                    output[out_len] = '\0';
                }
                deadline = time(NULL) + READ_TIMEOUT; /* reset on activity */
            } else if (n == SSH_EOF || n == 0) {
                eof_seen = 1;
            }
        }

        /* Stop early once channel is closed and no more data */
        if (ssh_channel_is_eof(channel)) {
            eof_seen = 1;
        }
    }

    if (verbose && out_len > 0) {
        printf("--- output ---\n%s\n--- end ---\n", output);
    }

    /* ------------------------------------------------------------------ */
    /* Evaluate result                                                     */
    /* ------------------------------------------------------------------ */
    int passed = 1;

    if (expect && !contains_icase(output, expect)) {
        fprintf(stderr, "FAIL: expected string '%s' not found in output\n", expect);
        if (!verbose && out_len > 0)
            fprintf(stderr, "output: %s\n", output);
        passed = 0;
    }

    /* ------------------------------------------------------------------ */
    /* Cleanup                                                             */
    /* ------------------------------------------------------------------ */
    ssh_channel_send_eof(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);
    free(output);

    return passed ? 0 : 1;
}
