/*
 * vms_mail.c - VMS MAIL utility for OVMX
 *
 * Implements a VMS-compatible electronic mail system for local inter-user
 * communication. Matches OpenVMS MAIL behavior including VMS-style prompts,
 * date formats, and error messages.
 *
 * Mail storage (rd vms-47fd): the user's mail file in the SYSUAF default
 * directory, read and appended through RMS over the executive ACP -- the SAME
 * file the DECnet MAIL-11 server (MAIL_SERVER.EXE) delivers NODE::USER mail
 * into and LOGINOUT counts. Format and helpers: vms_mail_notify.h,
 * mail_notify.c (read side), mail_store.c (write side).
 *
 * Build: part of tools/ CMakeLists.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#include "ovmx_layout.h"
#include "str_util.h"
#include "vmsfs/device.h"
#include "vms/logical.h"
/* SYSUAF is the sole VMS account database — binary indexed lookup (vms-d92). */
#include "sysuaf.h"
/* Whose mailbox this is comes from the executive (vms-a30). */
#include "vms_kif.h"
/* Shared mail storage layout + count/path helpers (vms-417): MAIL_SUBDIR,
 * MAIL_INDEX, get_user_homedir(), build_maildir(), mail_count_unread(). */
#include "vms_mail_notify.h"
#define MAX_MESSAGES    MAIL_STORE_MAX
#define MAX_BODY_LINES  2000
#define MAX_SUBJECT     256
#define MAX_USERNAME    64
#define MAX_LINE        4096
#define BODY_SENTINEL   "."  /* a line with just "." ends the body */

/* ------------------------------------------------------------------ */
/* The in-memory folder: the user's committed, undeleted messages      */
/* ------------------------------------------------------------------ */

typedef struct {
    int  number;              /* message number (1-based, positional, as VMS) */
    struct mail_store_entry m;
    int  read0, deleted0;     /* state as loaded: what save_marks() appends */
} mail_entry_t;

static mail_entry_t g_messages[MAX_MESSAGES];
static struct mail_store_entry g_load[MAX_MESSAGES];
static int          g_msg_count  = 0;
static int          g_current    = 0;   /* current message (1-based, 0=none) */
static char         g_username[MAX_USERNAME]; /* current user (uppercase) */
static int          g_dirty = 0;        /* read/deleted marks to append */

/* ------------------------------------------------------------------ */
/* SYSUAF user lookup (verify recipient exists)                        */
/* ------------------------------------------------------------------ */

static int user_exists(const char *username)
{
    /*
     * SYSUAF is the sole VMS account database. The lookup is a binary
     * indexed read (vms-d92) -- no ASCII pipe-parse. Existence is simply
     * "the engine returned a record for this name".
     *
     * DELETED, NOT REPLACED (vms-a30): a getpwnam(lowercased name) fall
     * back to /etc/passwd stood here. SYSUAF is the VMS account database;
     * consulting the host passwd file made every Linux login a VMS user
     * as far as MAIL was concerned. A VMS account that is not in SYSUAF
     * does not exist (Rule 10 -- match VMS or make the condition
     * unreachable; a local guess is neither).
     */
    sysuaf_record_t rec;
    if (sysuaf_lookup(username, &rec) == 0)
        return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mail file: load, and append the read/deleted marks on exit          */
/* ------------------------------------------------------------------ */

static void load_index(void)
{
    int n = 0;
    g_msg_count = 0;
    if (mail_store_load(g_username, g_load, MAX_MESSAGES, &n) != 0)
        return;
    for (int i = 0; i < n; i++) {
        if (g_load[i].deleted) continue;
        mail_entry_t *e = &g_messages[g_msg_count];
        e->m = g_load[i];
        e->number = ++g_msg_count;
        e->read0 = e->m.read;
        e->deleted0 = 0;
    }
}

static void save_index(void)
{
    for (int i = 0; i < g_msg_count; i++) {
        mail_entry_t *e = &g_messages[i];
        if ((e->m.read && !e->read0 && mail_store_mark(g_username, e->m.id, 'R') != 0) ||
            (e->m.deleted && !e->deleted0 && mail_store_mark(g_username, e->m.id, 'D') != 0)) {
            fprintf(stderr, "%%MAIL-E-WRITEERR, error writing mail file\n");
            return;
        }
        e->read0 = e->m.read;
        e->deleted0 = e->m.deleted;
    }
    g_dirty = 0;
}

/* The folder a message shows in: unread mail is NEWMAIL, the rest MAIL. */
static const char *folder_of(const mail_entry_t *e)
{
    return e->m.read ? "MAIL" : "NEWMAIL";
}

/* ------------------------------------------------------------------ */
/* Delivery: one message into a recipient's mail file (local SEND)     */
/* ------------------------------------------------------------------ */

static int deliver_message(const char *recipient_upper,
                           const char *sender_upper,
                           const char *subject,
                           const char *body)
{
    /* Split the body into lines: one record each in the mail file. */
    static const char *lines[MAX_BODY_LINES];
    unsigned n = 0;
    char *copy = strdup(body ? body : "");
    if (!copy) {
        fprintf(stderr, "%%MAIL-E-NOMEM, out of memory\n");
        return -1;
    }
    char *p = copy;
    while (*p && n < MAX_BODY_LINES) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t l = strlen(p);
        if (l && p[l - 1] == '\r') p[l - 1] = '\0';
        lines[n++] = p;
        if (!nl) break;
        p = nl + 1;
    }
    char err[256];
    uint32_t st = mail_store_deliver(recipient_upper, sender_upper, recipient_upper,
                                     "", subject, lines, n, err, sizeof err);
    free(copy);
    if (!(st & 1)) {
        fprintf(stderr, "%s\n", err[0] ? err : "%MAIL-E-CANTDELIVER, message not delivered");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DIRECTORY command -- list messages, in the VMS MAIL layout          */
/* ------------------------------------------------------------------ */

/* "8-OCT-2026 06:37:49.01" -> "8-OCT-2026" */
static void date_part(const char *full, char *out, size_t sz)
{
    snprintf(out, sz, "%s", full);
    char *sp = strchr(out, ' ');
    if (sp) *sp = '\0';
}

static void cmd_directory(void)
{
    int visible = 0, unread = 0;
    for (int i = 0; i < g_msg_count; i++) {
        if (g_messages[i].m.deleted) continue;
        visible++;
        if (!g_messages[i].m.read) unread++;
    }

    if (visible == 0) {
        printf("%%MAIL-I-NMSGS, no messages in MAIL\n");
        return;
    }

    printf("%76s\n", unread ? "NEWMAIL" : "MAIL");
    printf("    # From                 Date         Subject\n\n");
    for (int i = 0; i < g_msg_count; i++) {
        mail_entry_t *e = &g_messages[i];
        if (e->m.deleted) continue;
        char d[MAIL_DATE_MAX];
        date_part(e->m.date, d, sizeof d);
        printf("%5d %-20.20s %11s  %s\n", e->number, e->m.from, d, e->m.subj);
    }
}

/* ------------------------------------------------------------------ */
/* READ command -- display a message                                   */
/* ------------------------------------------------------------------ */

static void print_line_cb(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
}

static void cmd_read(int number)
{
    /* If number == 0, find next unread */
    if (number == 0) {
        for (int i = 0; i < g_msg_count; i++) {
            if (!g_messages[i].m.deleted && !g_messages[i].m.read) {
                number = g_messages[i].number;
                break;
            }
        }
        if (number == 0) {
            /* No unread -- read next after current */
            int found_current = 0;
            for (int i = 0; i < g_msg_count; i++) {
                if (g_messages[i].m.deleted) continue;
                if (found_current) { number = g_messages[i].number; break; }
                if (g_messages[i].number == g_current) found_current = 1;
            }
            if (number == 0) {
                printf("%%MAIL-I-NOMOREMSG, no more messages\n");
                return;
            }
        }
    }

    mail_entry_t *entry = NULL;
    for (int i = 0; i < g_msg_count; i++) {
        if (g_messages[i].number == number) {
            if (g_messages[i].m.deleted) {
                printf("%%MAIL-E-MSGNF, message %d has been deleted\n", number);
                return;
            }
            entry = &g_messages[i];
            break;
        }
    }
    if (!entry) {
        printf("%%MAIL-E-MSGNF, no such message number %d\n", number);
        return;
    }

    /* The VMS READ header: "    #1           8-OCT-2026 06:37:49.01 ... NEWMAIL" */
    char d[MAIL_DATE_MAX], t[MAIL_DATE_MAX] = "";
    date_part(entry->m.date, d, sizeof d);
    const char *sp = strchr(entry->m.date, ' ');
    if (sp) snprintf(t, sizeof t, "%s", sp + 1);
    printf("    #%-11d%11s %-11s%41s\n", number, d, t, folder_of(entry));
    printf("From:   %s\n", entry->m.from);
    printf("To:     %s\n", entry->m.to);
    if (entry->m.cc[0]) printf("CC:     %s\n", entry->m.cc);
    else                printf("CC:\n");
    printf("Subj:   %s\n", entry->m.subj);
    printf("\n");
    if (mail_store_body(g_username, entry->m.id, print_line_cb, NULL) < 0) {
        printf("%%MAIL-E-MSGNF, cannot read message %d\n", number);
        return;
    }

    /* Mark read */
    entry->m.read = 1;
    g_current = number;
    g_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* DELETE command                                                      */
/* ------------------------------------------------------------------ */

static void cmd_delete(int number)
{
    if (number == 0) number = g_current;
    if (number == 0) {
        printf("%%MAIL-E-MSGNF, no current message\n");
        return;
    }

    for (int i = 0; i < g_msg_count; i++) {
        if (g_messages[i].number == number) {
            if (g_messages[i].m.deleted) {
                printf("%%MAIL-E-MSGNF, message %d already deleted\n", number);
                return;
            }
            g_messages[i].m.deleted = 1;
            g_dirty = 1;
            printf("%%MAIL-S-DELETED, message %d deleted\n", number);
            return;
        }
    }
    printf("%%MAIL-E-MSGNF, no such message number %d\n", number);
}

/* ------------------------------------------------------------------ */
/* SEND command — compose and send a message                          */
/* ------------------------------------------------------------------ */

static void cmd_send(const char *preset_to, const char *preset_subject,
                     const char *body_from_stdin)
{
    char to[MAX_USERNAME];
    char subject[MAX_SUBJECT];

    /* Prompt for To: */
    if (preset_to && preset_to[0]) {
        strncpy(to, preset_to, sizeof(to) - 1);
        to[sizeof(to) - 1] = '\0';
        str_upcase(to);
    } else {
        printf("To: ");
        fflush(stdout);
        if (!fgets(to, sizeof(to), stdin)) return;
        str_trim(to);
        str_upcase(to);
    }

    if (to[0] == '\0') {
        printf("%%MAIL-E-NOTO, no recipient specified\n");
        return;
    }

    /* Verify recipient */
    if (!user_exists(to)) {
        printf("%%MAIL-E-NOSUCHUSR, no such user %s\n", to);
        return;
    }

    /* Prompt for Subject: */
    if (preset_subject && preset_subject[0]) {
        strncpy(subject, preset_subject, sizeof(subject) - 1);
        subject[sizeof(subject) - 1] = '\0';
    } else {
        printf("Subject: ");
        fflush(stdout);
        if (!fgets(subject, sizeof(subject), stdin)) return;
        str_trim(subject);
    }

    /* Read body */
    char *body = NULL;
    size_t body_len = 0;
    size_t body_cap = 0;

    if (body_from_stdin) {
        /* Non-interactive: use provided body */
        body_len = strlen(body_from_stdin);
        body = malloc(body_len + 1);
        if (!body) { perror("malloc"); return; }
        memcpy(body, body_from_stdin, body_len + 1);
    } else {
        /* Interactive: read until Ctrl-Z (EOF) or a line containing just "." */
        printf("Enter message body. End with Ctrl-Z or a line containing only '.':\n");
        char line[MAX_LINE];
        while (1) {
            if (!fgets(line, sizeof(line), stdin)) break; /* EOF / Ctrl-Z */
            str_trim(line);
            if (strcmp(line, BODY_SENTINEL) == 0) break;

            /* Append line + newline to body */
            size_t ll = strlen(line);
            size_t need = body_len + ll + 2;
            if (need > body_cap) {
                body_cap = need * 2 + 256;
                char *nb = realloc(body, body_cap);
                if (!nb) { perror("realloc"); free(body); return; }
                body = nb;
            }
            memcpy(body + body_len, line, ll);
            body_len += ll;
            body[body_len++] = '\n';
            body[body_len] = '\0';
        }
    }

    if (!body) {
        body = strdup("");
        body_len = 0;
    }

    /* Deliver */
    if (deliver_message(to, g_username, subject, body) == 0) {
        printf("%%MAIL-S-SENT, message sent to %s\n", to);
    }
    free(body);
}

/* ------------------------------------------------------------------ */
/* REPLY command                                                       */
/* ------------------------------------------------------------------ */

static void cmd_reply(void)
{
    if (g_current == 0) {
        printf("%%MAIL-E-NOMSGS, no current message to reply to\n");
        return;
    }

    /* Find current message entry */
    mail_entry_t *entry = NULL;
    for (int i = 0; i < g_msg_count; i++) {
        if (g_messages[i].number == g_current && !g_messages[i].m.deleted) {
            entry = &g_messages[i];
            break;
        }
    }
    if (!entry) {
        printf("%%MAIL-E-MSGNF, current message not available\n");
        return;
    }

    /* Build reply subject */
    char reply_subject[MAX_SUBJECT + 4];
    if (strncasecmp(entry->m.subj, "RE: ", 4) == 0) {
        snprintf(reply_subject, sizeof(reply_subject), "%.255s", entry->m.subj);
    } else {
        snprintf(reply_subject, sizeof(reply_subject), "RE: %.251s", entry->m.subj);
    }

    printf("Replying to message from %s\n", entry->m.from);
    cmd_send(entry->m.from, reply_subject, NULL);
}

/* ------------------------------------------------------------------ */
/* HELP command (in MAIL> context)                                     */
/* ------------------------------------------------------------------ */

static void cmd_help(void)
{
    printf("\n");
    printf("MAIL commands:\n\n");
    printf("  SEND               Compose and send a message\n");
    printf("  READ [n]           Display message n (or next unread)\n");
    printf("  DIRECTORY          List messages in mailbox\n");
    printf("  DELETE [n]         Delete message n (or current)\n");
    printf("  REPLY              Reply to current message\n");
    printf("  EXIT               Exit MAIL\n");
    printf("  QUIT               Exit MAIL\n");
    printf("  HELP               Show this help\n");
    printf("\n");
    printf("Press Return at 'To:' prompt to cancel SEND.\n");
    printf("End message body with Ctrl-Z or a line containing only '.'.\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Interactive MAIL> loop                                              */
/* ------------------------------------------------------------------ */

static void interactive_loop(void)
{
    char line[MAX_LINE];

    /* Show unread count at entry */
    int unread = 0;
    for (int i = 0; i < g_msg_count; i++) {
        if (!g_messages[i].m.deleted && !g_messages[i].m.read)
            unread++;
    }
    if (unread > 0) {
        printf("You have %d new mail message%s.\n\n", unread,
               unread != 1 ? "s" : "");
    }
    if (g_msg_count == 0 || (g_msg_count - unread == g_msg_count && unread == 0)) {
        /* No messages at all */
        if (g_msg_count == 0) {
            printf("Your mail file is empty.\n\n");
        }
    }

    while (1) {
        printf("MAIL> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break; /* EOF */
        str_trim(line);

        /* Skip empty lines */
        if (line[0] == '\0') continue;

        /* Parse verb + optional argument */
        char verb[64];
        char arg[MAX_LINE];
        arg[0] = '\0';

        int n = sscanf(line, "%63s %4095[^\n]", verb, arg);
        (void)n;
        str_upcase(verb);

        /* Minimum abbreviation matching (VMS style, 3-4 char minimum) */
        if (strncmp(verb, "SEND", 3) == 0) {
            cmd_send(NULL, NULL, NULL);
        } else if (strncmp(verb, "READ", 3) == 0 ||
                   strncmp(verb, "NEXT", 3) == 0) {
            int num = 0;
            if (arg[0] != '\0') num = atoi(arg);
            cmd_read(num);
        } else if (strncmp(verb, "DIRECTORY", 3) == 0 ||
                   strncmp(verb, "DIR", 3) == 0) {
            cmd_directory();
        } else if (strncmp(verb, "DELETE", 3) == 0) {
            int num = 0;
            if (arg[0] != '\0') num = atoi(arg);
            cmd_delete(num);
        } else if (strncmp(verb, "REPLY", 3) == 0) {
            cmd_reply();
        } else if (strncmp(verb, "HELP", 3) == 0 ||
                   verb[0] == '?') {
            cmd_help();
        } else if (strncmp(verb, "EXIT", 3) == 0 ||
                   strncmp(verb, "QUIT", 3) == 0) {
            break;
        } else {
            printf("%%MAIL-E-IVVERB, unrecognized MAIL command - \\%s\\\n", verb);
            printf("  Type HELP for list of available commands.\n");
        }
    }

    /* Save index if modified */
    if (g_dirty) {
        save_index();
    }
}

/* mail_count_unread() moved to tools/mail_notify.c (vms-417) -- see the
 * "Mail directory / index management" note above. */

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* Bootstrap VMS namespace */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    /*
     * ============================================================
     * WHOSE MAILBOX THIS IS COMES FROM THE EXECUTIVE (vms-a30)
     * ============================================================
     * g_username feeds build_maildir() directly, so this is the value
     * that PICKS THE MAILBOX. It is a real identity decision and it is
     * the executive's to make (CLAUDE.md Rule 11).
     *
     * WHAT USED TO STAND HERE, so nobody puts it back: a three-step
     * chain -- getenv("VMS_USERNAME"), then getpwuid(getuid()) upcased,
     * then the literal "SYSTEM". It is the same shape deleted from
     * lex_user(), F$IDENTIFIER and sys$sndopr, and MAIL.EXE was the last
     * binary carrying it. Every step is a value the process itself
     * controls or a host fact that is not a VMS identity: any caller
     * that could set VMS_USERNAME could open another user's mail, and
     * falling back to the local passwd file lets a Linux account decide
     * a VMS question. That is CLAUDE.md Rule 10's illegal third answer.
     *
     * THERE IS NO ABSENT-EXECUTIVE BRANCH AND MUST NOT BE ONE, and in
     * particular no message is invented for one. "The executive did not
     * answer" is the per-call condition vms-a35/vms-0ff deleted rather
     * than handled product-wide, on the ground that PID 1 refuses to
     * bring OVMX up without /dev/vms and holds it open for the life of
     * the system (src/ovmx_init/ovmx_init.c, executive_attach). Inventing
     * a %MAIL-F- or %OVMX-F- message here would repeat the EXECDEV /
     * NODEVTAB mistake recorded in src/vmsdcl/dcl_cmd_show.c. So on the
     * one OVMX runtime this cannot fail; off it, MAIL exits without
     * opening anything rather than guessing whose mail to open.
     * ============================================================
     */
    struct vms_procinfo self;
    memset(&self, 0, sizeof(self));
    if (!(vms_kif_getjpi_self(&self) & 1) || self.username[0] == '\0')
        return 1;

    strncpy(g_username, self.username, sizeof(g_username) - 1);
    g_username[sizeof(g_username) - 1] = '\0';
    str_upcase(g_username);

    /* The user's mail file is found through SYSUAF; no account, no mail. */
    {
        char spec[600];
        if (mail_store_spec(g_username, spec, sizeof spec) != 0) {
            fprintf(stderr, "%%MAIL-E-NOSUCHUSR, no such user %s\n", g_username);
            return 1;
        }
    }

    /* Load current user's index */
    load_index();

    /* Parse command-line arguments */
    /* Usage:
     *   vms_mail                          - interactive mode
     *   vms_mail /SUBJECT="text" recipient - send stdin to recipient
     */

    int send_mode = 0;
    const char *send_subject = NULL;
    const char *send_to = NULL;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strncasecmp(a, "/SUBJECT=", 9) == 0 ||
            strncasecmp(a, "-SUBJECT=", 9) == 0) {
            send_mode = 1;
            send_subject = a + 9;
            /* Strip surrounding quotes */
            if (send_subject[0] == '"') {
                send_subject++;
                /* We'll strip trailing quote below */
            }
        } else if (a[0] != '/' && a[0] != '-' && send_to == NULL) {
            send_to = a;
        }
    }

    if (send_mode && send_to) {
        /* Non-interactive send: read body from stdin */
        char to_upper[MAX_USERNAME];
        strncpy(to_upper, send_to, sizeof(to_upper) - 1);
        to_upper[sizeof(to_upper) - 1] = '\0';
        str_upcase(to_upper);

        char subj[MAX_SUBJECT] = "";
        if (send_subject) {
            strncpy(subj, send_subject, sizeof(subj) - 1);
            subj[sizeof(subj) - 1] = '\0';
            /* Strip trailing quote if present */
            size_t sl = strlen(subj);
            if (sl > 0 && subj[sl - 1] == '"') subj[sl - 1] = '\0';
        }

        /* Verify recipient */
        if (!user_exists(to_upper)) {
            fprintf(stderr, "%%MAIL-E-NOSUCHUSR, no such user %s\n", to_upper);
            return 1;
        }

        /* Read body from stdin */
        char *body = NULL;
        size_t body_len = 0;
        size_t body_cap = 0;
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), stdin)) {
            size_t ll = strlen(line);
            size_t need = body_len + ll + 1;
            if (need > body_cap) {
                body_cap = need * 2 + 256;
                char *nb = realloc(body, body_cap);
                if (!nb) { perror("realloc"); free(body); return 1; }
                body = nb;
            }
            memcpy(body + body_len, line, ll);
            body_len += ll;
            body[body_len] = '\0';
        }
        if (!body) body = strdup("");

        int rc = deliver_message(to_upper, g_username, subj, body);
        free(body);
        return (rc == 0) ? 0 : 1;
    }

    /* Interactive mode */
    printf("\n");
    printf("   MAIL -- OpenVMS Mail Utility\n\n");

    interactive_loop();

    printf("\n   Exiting MAIL\n\n");
    return 0;
}
