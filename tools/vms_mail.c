/*
 * vms_mail.c - VMS MAIL utility for OVMX
 *
 * Implements a VMS-compatible electronic mail system for local inter-user
 * communication. Matches OpenVMS MAIL behavior including VMS-style prompts,
 * date formats, and error messages.
 *
 * Mail storage:
 *   ~/.vmsmail/msg_NNNN.txt   - individual message files
 *   ~/.vmsmail/MAIL.IDX       - index file (message list with read/unread)
 *
 * Message file format:
 *   From: SENDER
 *   To: RECIPIENT
 *   Date: DD-MON-YYYY HH:MM:SS.CC
 *   Subject: text
 *   <blank line>
 *   body...
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
#include <pwd.h>
#include <dirent.h>
#include <fcntl.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#include "ovmx_layout.h"
#include "str_util.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "vms/logical.h"
#define SYSUAF_PATH     VMS_SYSUAF_PATH
#define MAIL_SUBDIR     ".vmsmail"
#define MAIL_INDEX      "MAIL.IDX"
#define MAX_MESSAGES    1000
#define MAX_SUBJECT     256
#define MAX_USERNAME    64
#define MAX_LINE        4096
#define BODY_SENTINEL   "."  /* a line with just "." ends the body */

/* VMS month names */
static const char *vms_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/* ------------------------------------------------------------------ */
/* Message index entry                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int  number;              /* message number (1-based) */
    char from[MAX_USERNAME];  /* sender (uppercase) */
    char date[32];            /* VMS-format date string */
    char subject[MAX_SUBJECT];
    int  read;                /* 0 = unread, 1 = read */
    int  deleted;             /* 0 = present, 1 = deleted */
} mail_entry_t;

/* In-memory message list */
static mail_entry_t g_messages[MAX_MESSAGES];
static int          g_msg_count  = 0;
static int          g_current    = 0;   /* current message (1-based, 0=none) */
static char         g_maildir[4096];    /* absolute path to ~/.vmsmail */
static char         g_username[MAX_USERNAME]; /* current user (uppercase) */
static int          g_dirty = 0;        /* index needs rewriting */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* str_upcase() and str_trim() replaced by str_str_upcase()/str_trim() from str_util.h */

/* Format current time as VMS date: DD-MON-YYYY HH:MM:SS.CC */
static void vms_now(char *buf, size_t bufsiz)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int cc = (int)(ts.tv_nsec / 10000000);
    snprintf(buf, bufsiz, "%02d-%s-%04d %02d:%02d:%02d.%02d",
             tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
             tm.tm_hour, tm.tm_min, tm.tm_sec, cc);
}

/* Short date only: DD-MON-YYYY */
static void vms_date_short(char *buf, size_t bufsiz)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(buf, bufsiz, "%02d-%s-%04d",
             tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year);
}

/* ------------------------------------------------------------------ */
/* SYSUAF user lookup (verify recipient exists)                        */
/* ------------------------------------------------------------------ */

static int user_exists(const char *username)
{
    /* First check sysuaf.dat */
    char sysuaf_linux[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux, sizeof(sysuaf_linux));
    FILE *fp = fopen(sysuaf_linux, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                continue;
            str_trim(line);
            /* First field is username (SYSUAF uses pipe delimiter) */
            char *delim = strchr(line, '|');
            if (delim) *delim = '\0';
            char uname[MAX_USERNAME];
            strncpy(uname, line, sizeof(uname) - 1);
            uname[sizeof(uname) - 1] = '\0';
            str_upcase(uname);
            char search[MAX_USERNAME];
            strncpy(search, username, sizeof(search) - 1);
            search[sizeof(search) - 1] = '\0';
            str_upcase(search);
            if (strcmp(uname, search) == 0) {
                fclose(fp);
                return 1;
            }
        }
        fclose(fp);
    }

    /* Fall back to /etc/passwd for Linux users */
    char lower[MAX_USERNAME];
    strncpy(lower, username, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (int i = 0; lower[i]; i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    return (getpwnam(lower) != NULL);
}

/* Get home directory for a VMS username */
static int get_user_homedir(const char *username, char *homedir, size_t sz)
{
    /* Try sysuaf.dat first (default_dir field) */
    char sysuaf_linux2[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux2, sizeof(sysuaf_linux2));
    FILE *fp = fopen(sysuaf_linux2, "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                continue;
            str_trim(line);
            char *fields[7];
            char *p = line;
            int nf = 0;
            for (nf = 0; nf < 7 && p; nf++) {
                fields[nf] = p;
                char *delim = strchr(p, '|');
                if (delim) { *delim = '\0'; p = delim + 1; }
                else p = NULL;
            }
            if (nf < 5) continue;
            char uname[MAX_USERNAME];
            strncpy(uname, fields[0], sizeof(uname) - 1);
            uname[sizeof(uname) - 1] = '\0';
            str_upcase(uname);
            char search[MAX_USERNAME];
            strncpy(search, username, sizeof(search) - 1);
            search[sizeof(search) - 1] = '\0';
            str_upcase(search);
            if (strcmp(uname, search) == 0) {
                strncpy(homedir, fields[4], sz - 1);
                homedir[sz - 1] = '\0';
                fclose(fp);
                return 0;
            }
        }
        fclose(fp);
    }

    /* Fall back to /etc/passwd */
    char lower[MAX_USERNAME];
    strncpy(lower, username, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (int i = 0; lower[i]; i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    struct passwd *pw = getpwnam(lower);
    if (pw) {
        strncpy(homedir, pw->pw_dir, sz - 1);
        homedir[sz - 1] = '\0';
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Mail directory / index management                                   */
/* ------------------------------------------------------------------ */

/* Build maildir path for a given username */
static void build_maildir(const char *username, char *out, size_t sz)
{
    char homedir[4096];
    if (get_user_homedir(username, homedir, sizeof(homedir)) != 0) {
        /* Fallback */
        snprintf(homedir, sizeof(homedir), "/home/%s", username);
    }
    snprintf(out, sz, "%s/%s", homedir, MAIL_SUBDIR);
}

/* Ensure maildir exists */
static int ensure_maildir(const char *maildir)
{
    struct stat st;
    if (stat(maildir, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1;
    }
    if (mkdir(maildir, 0700) != 0) return -1;
    return 0;
}

/* Read index file into g_messages[] */
static void load_index(void)
{
    g_msg_count = 0;
    char idxpath[4096];
    snprintf(idxpath, sizeof(idxpath), "%s/%s", g_maildir, MAIL_INDEX);

    FILE *fp = fopen(idxpath, "r");
    if (!fp) return;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp) && g_msg_count < MAX_MESSAGES) {
        str_trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        /* Format: NUMBER|READ|DELETED|FROM|DATE|SUBJECT */
        mail_entry_t *e = &g_messages[g_msg_count];
        memset(e, 0, sizeof(*e));

        char *tok;
        char tmp[MAX_LINE];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        tok = strtok(tmp, "|");
        if (!tok) continue;
        e->number = atoi(tok);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        e->read = atoi(tok);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        e->deleted = atoi(tok);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        strncpy(e->from, tok, sizeof(e->from) - 1);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        strncpy(e->date, tok, sizeof(e->date) - 1);

        tok = strtok(NULL, "|");
        if (!tok) continue;
        strncpy(e->subject, tok, sizeof(e->subject) - 1);

        g_msg_count++;
    }
    fclose(fp);
}

/* Write index file from g_messages[] */
static void save_index(void)
{
    char idxpath[4096];
    snprintf(idxpath, sizeof(idxpath), "%s/%s", g_maildir, MAIL_INDEX);

    FILE *fp = fopen(idxpath, "w");
    if (!fp) {
        fprintf(stderr, "%%MAIL-E-CANTWRITE, cannot write index file\n");
        return;
    }
    fprintf(fp, "# OVMX MAIL index - do not edit manually\n");
    for (int i = 0; i < g_msg_count; i++) {
        mail_entry_t *e = &g_messages[i];
        fprintf(fp, "%d|%d|%d|%s|%s|%s\n",
                e->number, e->read, e->deleted,
                e->from, e->date, e->subject);
    }
    fclose(fp);
    g_dirty = 0;
}

/* Allocate next message number */
static int next_msg_number(void)
{
    int maxn = 0;
    for (int i = 0; i < g_msg_count; i++) {
        if (g_messages[i].number > maxn)
            maxn = g_messages[i].number;
    }
    return maxn + 1;
}

/* Get message file path for a given number */
static void msg_filepath(int number, char *out, size_t sz)
{
    snprintf(out, sz, "%s/msg_%04d.txt", g_maildir, number);
}

/* ------------------------------------------------------------------ */
/* Delivery: write a message into a recipient's mailbox               */
/* ------------------------------------------------------------------ */

static int deliver_message(const char *recipient_upper,
                           const char *sender_upper,
                           const char *subject,
                           const char *body)
{
    char recip_maildir[4096];
    build_maildir(recipient_upper, recip_maildir, sizeof(recip_maildir));
    if (ensure_maildir(recip_maildir) != 0) {
        fprintf(stderr, "%%MAIL-E-CANTDELIVER, cannot create mail directory for %s\n",
                recipient_upper);
        return -1;
    }

    /* Read recipient's current index to get next message number */
    char save_maildir[4096];
    mail_entry_t *save_msgs = malloc(MAX_MESSAGES * sizeof(mail_entry_t));
    if (!save_msgs) {
        fprintf(stderr, "%%MAIL-E-NOMEM, out of memory\n");
        return -1;
    }
    int save_count = g_msg_count;
    int save_current = g_current;

    /* Temporarily swap to recipient's maildir to get next msg# */
    strncpy(save_maildir, g_maildir, sizeof(save_maildir) - 1);
    save_maildir[sizeof(save_maildir) - 1] = '\0';
    memcpy(save_msgs, g_messages, g_msg_count * sizeof(mail_entry_t));

    strncpy(g_maildir, recip_maildir, sizeof(g_maildir) - 1);
    g_maildir[sizeof(g_maildir) - 1] = '\0';
    load_index();
    int new_num = next_msg_number();
    int recip_msg_count = g_msg_count;
    mail_entry_t *recip_msgs = malloc(MAX_MESSAGES * sizeof(mail_entry_t));
    if (!recip_msgs) {
        fprintf(stderr, "%%MAIL-E-NOMEM, out of memory\n");
        free(save_msgs);
        return -1;
    }
    memcpy(recip_msgs, g_messages, g_msg_count * sizeof(mail_entry_t));

    /* Restore sender's state */
    strncpy(g_maildir, save_maildir, sizeof(g_maildir) - 1);
    g_maildir[sizeof(g_maildir) - 1] = '\0';
    memcpy(g_messages, save_msgs, save_count * sizeof(mail_entry_t));
    g_msg_count = save_count;
    g_current = save_current;
    free(save_msgs);

    /* Bounds check: recipient mailbox full */
    if (recip_msg_count >= MAX_MESSAGES) {
        fprintf(stderr, "%%MAIL-E-MAILFULL, recipient mailbox is full\n");
        free(recip_msgs);
        return -1;
    }

    /* Write message file to recipient's maildir */
    char msgpath[4096];
    snprintf(msgpath, sizeof(msgpath), "%s/msg_%04d.txt", recip_maildir, new_num);
    FILE *fp = fopen(msgpath, "w");
    if (!fp) {
        fprintf(stderr, "%%MAIL-E-CANTWRITE, cannot write message file\n");
        free(recip_msgs);
        return -1;
    }

    char datebuf[64];
    vms_now(datebuf, sizeof(datebuf));

    fprintf(fp, "From: %s\n", sender_upper);
    fprintf(fp, "To: %s\n", recipient_upper);
    fprintf(fp, "Date: %s\n", datebuf);
    fprintf(fp, "Subject: %s\n", subject);
    fprintf(fp, "\n");
    fputs(body, fp);
    /* Ensure final newline */
    size_t blen = strlen(body);
    if (blen > 0 && body[blen - 1] != '\n')
        fprintf(fp, "\n");
    fclose(fp);

    /* Update recipient's index */
    mail_entry_t *ne = &recip_msgs[recip_msg_count];
    memset(ne, 0, sizeof(*ne));
    ne->number = new_num;
    ne->read = 0;
    ne->deleted = 0;
    strncpy(ne->from, sender_upper, sizeof(ne->from) - 1);

    /* Short date for index */
    char sdate[32];
    vms_date_short(sdate, sizeof(sdate));
    strncpy(ne->date, sdate, sizeof(ne->date) - 1);
    strncpy(ne->subject, subject, sizeof(ne->subject) - 1);
    recip_msg_count++;

    /* Write updated recipient index */
    char idxpath[4096];
    snprintf(idxpath, sizeof(idxpath), "%s/%s", recip_maildir, MAIL_INDEX);
    FILE *ifp = fopen(idxpath, "w");
    if (!ifp) {
        fprintf(stderr, "%%MAIL-E-CANTWRITE, cannot write recipient index\n");
        free(recip_msgs);
        return -1;
    }
    fprintf(ifp, "# OVMX MAIL index - do not edit manually\n");
    for (int i = 0; i < recip_msg_count; i++) {
        mail_entry_t *e = &recip_msgs[i];
        fprintf(ifp, "%d|%d|%d|%s|%s|%s\n",
                e->number, e->read, e->deleted,
                e->from, e->date, e->subject);
    }
    fclose(ifp);

    free(recip_msgs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* DIRECTORY command — list messages                                   */
/* ------------------------------------------------------------------ */

static void cmd_directory(void)
{
    /* Count non-deleted messages */
    int visible = 0;
    for (int i = 0; i < g_msg_count; i++) {
        if (!g_messages[i].deleted) visible++;
    }

    if (visible == 0) {
        printf("%%MAIL-I-NMSGS, no messages in MAIL\n");
        return;
    }

    printf("\n");
    printf("  #  %-16s  %-14s  %s\n", "From", "Date", "Subject");
    printf("  %s\n", "--------------------------------------------------------------------------------");

    for (int i = 0; i < g_msg_count; i++) {
        mail_entry_t *e = &g_messages[i];
        if (e->deleted) continue;
        char flag = e->read ? ' ' : '*';
        printf(" %c%2d  %-16s  %-14s  %s\n",
               flag, e->number, e->from, e->date, e->subject);
    }
    printf("\n");
    printf("  (* = unread)\n\n");
}

/* ------------------------------------------------------------------ */
/* READ command — display a message                                    */
/* ------------------------------------------------------------------ */

static void cmd_read(int number)
{
    /* If number == 0, find next unread */
    if (number == 0) {
        for (int i = 0; i < g_msg_count; i++) {
            if (!g_messages[i].deleted && !g_messages[i].read) {
                number = g_messages[i].number;
                break;
            }
        }
        if (number == 0) {
            /* No unread — read next after current */
            int found_current = 0;
            for (int i = 0; i < g_msg_count; i++) {
                if (g_messages[i].deleted) continue;
                if (found_current) { number = g_messages[i].number; break; }
                if (g_messages[i].number == g_current) found_current = 1;
            }
            if (number == 0) {
                printf("%%MAIL-I-NOMOREMSG, no more messages\n");
                return;
            }
        }
    }

    /* Find the entry */
    mail_entry_t *entry = NULL;
    for (int i = 0; i < g_msg_count; i++) {
        if (g_messages[i].number == number) {
            if (g_messages[i].deleted) {
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

    /* Open message file */
    char msgpath[4096];
    msg_filepath(number, msgpath, sizeof(msgpath));
    FILE *fp = fopen(msgpath, "r");
    if (!fp) {
        printf("%%MAIL-E-MSGNF, cannot open message file for message %d\n", number);
        return;
    }

    printf("\n");
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        fputs(line, stdout);
    }
    printf("\n");
    fclose(fp);

    /* Mark read */
    entry->read = 1;
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
            if (g_messages[i].deleted) {
                printf("%%MAIL-E-MSGNF, message %d already deleted\n", number);
                return;
            }
            g_messages[i].deleted = 1;
            g_dirty = 1;

            /* Remove message file */
            char msgpath[4096];
            msg_filepath(number, msgpath, sizeof(msgpath));
            unlink(msgpath);

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
        printf("%%MAIL-E-NOSUCHUSER, no such user %s\n", to);
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
        if (g_messages[i].number == g_current && !g_messages[i].deleted) {
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
    if (strncasecmp(entry->subject, "RE: ", 4) == 0) {
        snprintf(reply_subject, sizeof(reply_subject), "%s", entry->subject);
    } else {
        snprintf(reply_subject, sizeof(reply_subject), "RE: %s", entry->subject);
    }

    printf("Replying to message from %s\n", entry->from);
    cmd_send(entry->from, reply_subject, NULL);
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
        if (!g_messages[i].deleted && !g_messages[i].read)
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

/* ------------------------------------------------------------------ */
/* New-mail check function (for use by login notification)            */
/* ------------------------------------------------------------------ */

int mail_count_unread(const char *username)
{
    char maildir[4096];
    build_maildir(username, maildir, sizeof(maildir));

    char idxpath[4096];
    snprintf(idxpath, sizeof(idxpath), "%s/%s", maildir, MAIL_INDEX);

    FILE *fp = fopen(idxpath, "r");
    if (!fp) return 0;

    int count = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        str_trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        /* Parse: NUMBER|READ|DELETED|... */
        char tmp[MAX_LINE];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *tok = strtok(tmp, "|");
        if (!tok) continue; /* number */
        tok = strtok(NULL, "|");
        if (!tok) continue;
        int read = atoi(tok);
        tok = strtok(NULL, "|");
        if (!tok) continue;
        int deleted = atoi(tok);

        if (!read && !deleted) count++;
    }
    fclose(fp);
    return count;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* Bootstrap VMS namespace */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    /* Determine current username */
    const char *env_user = getenv("VMS_USERNAME");
    if (env_user && env_user[0]) {
        strncpy(g_username, env_user, sizeof(g_username) - 1);
        g_username[sizeof(g_username) - 1] = '\0';
        str_upcase(g_username);
    } else {
        struct passwd *pw = getpwuid(getuid());
        if (pw) {
            strncpy(g_username, pw->pw_name, sizeof(g_username) - 1);
            g_username[sizeof(g_username) - 1] = '\0';
            str_upcase(g_username);
        } else {
            strncpy(g_username, "SYSTEM", sizeof(g_username) - 1);
        }
    }

    /* Build maildir for current user */
    build_maildir(g_username, g_maildir, sizeof(g_maildir));
    if (ensure_maildir(g_maildir) != 0) {
        fprintf(stderr, "%%MAIL-E-NOMAIL, cannot create mail directory: %s\n",
                strerror(errno));
        return 1;
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
            fprintf(stderr, "%%MAIL-E-NOSUCHUSER, no such user %s\n", to_upper);
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
