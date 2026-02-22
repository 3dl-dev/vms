/*
 * lnm_daemon.c - Logical Name Daemon
 *
 * Provides system-wide logical name state via a Unix domain socket.
 * When compiled with LNM_DAEMON_MAIN, runs as a standalone daemon.
 * Otherwise, the file is included in the library build but the
 * main() entry point is omitted.
 *
 * Protocol (newline-delimited text commands):
 *   CREATE <table> <name> <value>
 *   DELETE <table> <name>
 *   TRANSLATE <table> <name>
 *   ENUMERATE <table>
 *
 * Responses:
 *   OK <data>         -- success
 *   ERR <status_hex>  -- failure with VMS status code
 *   ENTRY <name> <value>  -- for ENUMERATE, one per entry
 *   END               -- end of ENUMERATE results
 *
 * Configuration:
 *   Reads /etc/ovmx/sylogicals.conf on startup (one "name value" per line).
 *   Listens on /tmp/ovmx/lnm.sock.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>

#include "vms/logical.h"
#include "ssdef.h"

#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#define LNM_SOCKET_PATH  "/tmp/ovmx/lnm.sock"
#define LNM_CONF_PATH    VMS_LNM_CONF_PATH
#define LNM_LINE_MAX     1024
#define LNM_VMS_ROOT     SYSDISK_MOUNT

#ifdef LNM_DAEMON_MAIN

static volatile int daemon_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    daemon_running = 0;
}

/*
 * Enumeration callback used by the ENUMERATE command.
 * Writes one "ENTRY name value\n" line per logical name to the client fd.
 */
struct enum_ctx {
    int fd;
};

static int enum_callback(const char *name, const lnm_entry_t *entry, void *ctx)
{
    struct enum_ctx *ec = (struct enum_ctx *)ctx;
    char line[LNM_LINE_MAX];
    const char *value = "";

    if (entry->num_translations > 0)
        value = entry->translations[0].value;

    int n = snprintf(line, sizeof(line), "ENTRY %s %s\n", name, value);
    if (n > 0)
        (void)write(ec->fd, line, (size_t)n);

    return 0; /* continue */
}

/*
 * Parse and execute a single command from a client.
 */
static void process_command(lnm_manager_t *mgr, int client_fd, char *cmd)
{
    char response[LNM_LINE_MAX];
    char *saveptr = NULL;

    /* Trim trailing newline/carriage return */
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == '\n' || cmd[len - 1] == '\r'))
        cmd[--len] = '\0';

    if (len == 0)
        return;

    char *verb = strtok_r(cmd, " \t", &saveptr);
    if (!verb)
        return;

    if (strcasecmp(verb, "CREATE") == 0) {
        char *table = strtok_r(NULL, " \t", &saveptr);
        char *name  = strtok_r(NULL, " \t", &saveptr);
        /* The rest of the line is the value (may contain spaces) */
        char *value = saveptr;

        if (!table || !name || !value || *value == '\0') {
            snprintf(response, sizeof(response), "ERR %08X\n", SS$_BADPARAM);
        } else {
            /* Trim leading whitespace from value */
            while (*value == ' ' || *value == '\t')
                value++;
            uint32_t status = lnm_create(mgr, table, name, value,
                                          0, LNM_MODE_USER);
            if ($VMS_STATUS_SUCCESS(status))
                snprintf(response, sizeof(response), "OK\n");
            else
                snprintf(response, sizeof(response), "ERR %08X\n", status);
        }
        (void)write(client_fd, response, strlen(response));

    } else if (strcasecmp(verb, "DELETE") == 0) {
        char *table = strtok_r(NULL, " \t", &saveptr);
        char *name  = strtok_r(NULL, " \t", &saveptr);

        if (!table || !name) {
            snprintf(response, sizeof(response), "ERR %08X\n", SS$_BADPARAM);
        } else {
            uint32_t status = lnm_delete(mgr, table, name, LNM_MODE_USER);
            if ($VMS_STATUS_SUCCESS(status))
                snprintf(response, sizeof(response), "OK\n");
            else
                snprintf(response, sizeof(response), "ERR %08X\n", status);
        }
        (void)write(client_fd, response, strlen(response));

    } else if (strcasecmp(verb, "TRANSLATE") == 0) {
        char *table = strtok_r(NULL, " \t", &saveptr);
        char *name  = strtok_r(NULL, " \t", &saveptr);

        if (!table || !name) {
            snprintf(response, sizeof(response), "ERR %08X\n", SS$_BADPARAM);
        } else {
            char result[LNM_MAX_VALUE + 1];
            uint16_t result_len = 0;
            uint32_t attrs = 0;
            uint32_t status = lnm_translate(mgr, table, name,
                                             result, sizeof(result),
                                             &result_len, &attrs);
            if ($VMS_STATUS_SUCCESS(status))
                snprintf(response, sizeof(response), "OK %s\n", result);
            else
                snprintf(response, sizeof(response), "ERR %08X\n", status);
        }
        (void)write(client_fd, response, strlen(response));

    } else if (strcasecmp(verb, "ENUMERATE") == 0) {
        char *table = strtok_r(NULL, " \t", &saveptr);

        if (!table) {
            snprintf(response, sizeof(response), "ERR %08X\n", SS$_BADPARAM);
            (void)write(client_fd, response, strlen(response));
        } else {
            struct enum_ctx ctx;
            ctx.fd = client_fd;
            uint32_t status = lnm_enumerate(mgr, table, enum_callback, &ctx);
            if (!$VMS_STATUS_SUCCESS(status)) {
                snprintf(response, sizeof(response), "ERR %08X\n", status);
                (void)write(client_fd, response, strlen(response));
            }
            /* Send end-of-enumeration marker */
            (void)write(client_fd, "END\n", 4);
        }

    } else {
        snprintf(response, sizeof(response), "ERR %08X\n", SS$_IVSSRQ);
        (void)write(client_fd, response, strlen(response));
    }
}

/* Thread context for client handler */
struct client_ctx {
    int fd;
    lnm_manager_t *mgr;
};

/*
 * Client handler thread.
 * Reads newline-delimited commands and dispatches them.
 */
static void *handle_client(void *arg)
{
    struct client_ctx *ctx = (struct client_ctx *)arg;
    int client_fd = ctx->fd;
    lnm_manager_t *mgr = ctx->mgr;
    free(ctx);

    char buf[LNM_LINE_MAX];
    ssize_t nread;
    char linebuf[LNM_LINE_MAX];
    size_t linepos = 0;

    while ((nread = read(client_fd, buf, sizeof(buf) - 1)) > 0) {
        buf[nread] = '\0';

        /* Accumulate into line buffer, process complete lines */
        for (ssize_t i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                linebuf[linepos] = '\0';
                process_command(mgr, client_fd, linebuf);
                linepos = 0;
            } else if (linepos < sizeof(linebuf) - 1) {
                linebuf[linepos++] = buf[i];
            }
        }
    }

    close(client_fd);
    return NULL;
}

/*
 * Load system logicals from a configuration file.
 *
 * File format: one logical per line, "NAME VALUE" separated by whitespace.
 * Lines starting with '#' are comments. Blank lines are ignored.
 */
static void load_config(lnm_manager_t *mgr, const char *conf_path)
{
    FILE *fp = fopen(conf_path, "r");
    if (!fp) {
        /* Not an error -- config file is optional */
        return;
    }

    char line[LNM_LINE_MAX];
    while (fgets(line, sizeof(line), fp)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip blank lines and comments */
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        /* Parse "NAME VALUE" */
        char *name = strtok(p, " \t");
        char *value = strtok(NULL, "\n");
        if (!name || !value)
            continue;

        /* Trim leading whitespace from value */
        while (*value == ' ' || *value == '\t')
            value++;

        if (*value == '\0')
            continue;

        lnm_create(mgr, LNM_SYSTEM_TABLE, name, value,
                   LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
    }

    fclose(fp);
}

/*
 * Ensure the socket directory exists.
 */
static int ensure_socket_dir(const char *socket_path)
{
    /* Extract directory from socket path */
    char dir[256];
    strncpy(dir, socket_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(dir, 0755) < 0 && errno != EEXIST)
            return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("OVMX Logical Name Daemon starting...\n");

    /* Initialize the logical name manager */
    lnm_manager_t *mgr = lnm_init();
    if (!mgr) {
        fprintf(stderr, "Failed to initialize logical name manager\n");
        return 1;
    }

    /* Bootstrap device table before setting up logicals */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);

    /* Set up default VMS system logicals */
    lnm_setup_defaults(mgr, LNM_VMS_ROOT);

    /* Load additional system logicals from configuration file */
    char lnm_conf_linux[1024];
    vmsfs_to_linux_path(LNM_CONF_PATH, lnm_conf_linux, sizeof(lnm_conf_linux));
    load_config(mgr, lnm_conf_linux);

    /* Ensure socket directory exists */
    if (ensure_socket_dir(LNM_SOCKET_PATH) < 0) {
        fprintf(stderr, "Failed to create socket directory: %s\n",
                strerror(errno));
        lnm_shutdown(mgr);
        return 1;
    }

    /* Remove stale socket file */
    unlink(LNM_SOCKET_PATH);

    /* Create Unix domain socket */
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        lnm_shutdown(mgr);
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, LNM_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        lnm_shutdown(mgr);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        lnm_shutdown(mgr);
        return 1;
    }

    /* Set up signal handlers for clean shutdown */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    printf("Logical Name Daemon listening on %s\n", LNM_SOCKET_PATH);

    while (daemon_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        /*
         * Allocate a context block containing the client fd and
         * a pointer to the manager for the handler thread.
         */
        struct client_ctx *ctx = malloc(sizeof(struct client_ctx));
        if (!ctx) {
            close(client_fd);
            continue;
        }
        ctx->fd = client_fd;
        ctx->mgr = mgr;

        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, ctx) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(ctx);
            continue;
        }
        pthread_detach(thread);
    }

    printf("Logical Name Daemon shutting down...\n");

    close(server_fd);
    unlink(LNM_SOCKET_PATH);
    lnm_shutdown(mgr);

    printf("Logical Name Daemon stopped.\n");
    return 0;
}

#endif /* LNM_DAEMON_MAIN */
