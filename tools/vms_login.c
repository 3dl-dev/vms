/*
 * vms_login.c - VMS-style login program for OVMX
 *
 * Displays a VMS-like login banner, prompts for username and
 * password, authenticates against /etc/ovmx/sysuaf.dat, and
 * then execs the DCL shell with --login flag.
 *
 * Build: part of tools/ CMakeLists.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "sha256.h"
#include "vms/pcb.h"
#include "vms/privs.h"
#include "ovmx_accounting.h"

/* Maximum number of login attempts before disconnect */
#define MAX_ATTEMPTS   3

/* Paths */
#define SYSUAF_PATH        "/etc/ovmx/sysuaf.dat"
#ifndef OVMX_BIN_DIR
#define OVMX_BIN_DIR "/usr/local/bin"
#endif
#define DCL_SHELL_PATH     OVMX_BIN_DIR "/vmsdcl"

/* SYSUAF record */
typedef struct {
    char     username[64];
    char     password_hash[128];
    uint32_t uic_group;
    uint32_t uic_member;
    char     default_dir[256];
    char     flags[64];
    char     privileges[256];
} sysuaf_record_t;

/* ------------------------------------------------------------------ */
/* Helper: upcase a string in-place                                   */
/* ------------------------------------------------------------------ */
static void upcase(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

/* ------------------------------------------------------------------ */
/* Helper: trim trailing whitespace / newlines                        */
/* ------------------------------------------------------------------ */
static void trim_trailing(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t'))
        s[--len] = '\0';
}

/* ------------------------------------------------------------------ */
/* Read password with echo disabled                                   */
/* ------------------------------------------------------------------ */
static int read_password(char *buf, size_t bufsiz)
{
    struct termios old_term, new_term;
    int have_term = 0;

    /* Turn off echo (only if stdin is a terminal) */
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_term) == 0) {
        new_term = old_term;
        new_term.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_term);
        have_term = 1;
    }

    /* Read the line */
    if (fgets(buf, (int)bufsiz, stdin) == NULL) {
        if (have_term)
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
        return -1;
    }

    /* Restore terminal */
    if (have_term) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
        /* Print a newline (since echo was off) */
        putchar('\n');
    }

    trim_trailing(buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Look up a user in sysuaf.dat                                       */
/* ------------------------------------------------------------------ */
static int lookup_user(const char *username, sysuaf_record_t *rec)
{
    FILE *fp = fopen(SYSUAF_PATH, "r");
    if (!fp)
        return -1;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        trim_trailing(line);

        /* Parse: USERNAME:PASSWORD_HASH:UIC_GROUP:UIC_MEMBER:DEFAULT_DIR:FLAGS:PRIVILEGES */
        char *fields[7];
        char *p = line;
        int nf = 0;

        for (nf = 0; nf < 7 && p; nf++) {
            fields[nf] = p;
            char *colon = strchr(p, ':');
            if (colon) {
                *colon = '\0';
                p = colon + 1;
            } else {
                p = NULL;
            }
        }

        if (nf < 5)
            continue;  /* malformed line */

        /* Case-insensitive compare */
        char uname_copy[64];
        strncpy(uname_copy, fields[0], sizeof(uname_copy) - 1);
        uname_copy[sizeof(uname_copy) - 1] = '\0';
        upcase(uname_copy);

        char search_copy[64];
        strncpy(search_copy, username, sizeof(search_copy) - 1);
        search_copy[sizeof(search_copy) - 1] = '\0';
        upcase(search_copy);

        if (strcmp(uname_copy, search_copy) == 0) {
            strncpy(rec->username, fields[0], sizeof(rec->username) - 1);
            upcase(rec->username);
            strncpy(rec->password_hash, fields[1], sizeof(rec->password_hash) - 1);
            rec->uic_group  = (uint32_t)strtoul(fields[2], NULL, 10);
            rec->uic_member = (uint32_t)strtoul(fields[3], NULL, 10);
            strncpy(rec->default_dir, fields[4], sizeof(rec->default_dir) - 1);
            if (nf > 5)
                strncpy(rec->flags, fields[5], sizeof(rec->flags) - 1);
            if (nf > 6)
                strncpy(rec->privileges, fields[6], sizeof(rec->privileges) - 1);
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return -1;  /* User not found */
}

/* ------------------------------------------------------------------ */
/* Authenticate: compare supplied password against stored hash        */
/* Empty hash = no password required. Non-empty = SHA256 hex compare. */
/* ------------------------------------------------------------------ */
static int authenticate(const sysuaf_record_t *rec, const char *password)
{
    /* Empty hash = no password required */
    if (rec->password_hash[0] == '\0')
        return 1;

    /* Hash the supplied password with SHA256 */
    char hex[65];
    sha256_hex((const uint8_t *)password, strlen(password), hex);

    /* Compare against stored hex hash (case-insensitive) */
    return (strcasecmp(hex, rec->password_hash) == 0);
}

/* ------------------------------------------------------------------ */
/* Start a VMS session: banner, environment, exec DCL shell           */
/* ------------------------------------------------------------------ */
static void start_session(const sysuaf_record_t *rec)
{
    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    printf("\n   Welcome to OpenVMS (tm) OVMX V7.3\n");

    /* Read the REAL last login time from accounting records */
    time_t last_login = 0;
    int has_last = (ovmx_accounting_get_lastlogin(rec->username, &last_login) == 0);

    if (has_last && last_login > 0) {
        struct tm *tm = localtime(&last_login);
        printf("\n   Last interactive login on %02d-%s-%04d %02d:%02d:%02d\n\n",
               tm->tm_mday, months[tm->tm_mon], tm->tm_year + 1900,
               tm->tm_hour, tm->tm_min, tm->tm_sec);
    } else {
        printf("\n   No previous interactive login recorded.\n\n");
    }

    /* Record this login now (after showing last, before launching DCL) */
    ovmx_accounting_record_login(rec->username);

    /* Set up environment for session */
    setenv("VMS_USERNAME",    rec->username,    1);

    char uic_group_str[16], uic_member_str[16];
    snprintf(uic_group_str, sizeof(uic_group_str), "%u", rec->uic_group);
    snprintf(uic_member_str, sizeof(uic_member_str), "%u", rec->uic_member);
    setenv("VMS_UIC_GROUP",   uic_group_str, 1);
    setenv("VMS_UIC_MEMBER",  uic_member_str, 1);
    setenv("VMS_DEFAULT_DIR", rec->default_dir, 1);
    setenv("VMS_PRIVILEGES",  rec->privileges, 1);

    /* Build logical name equivalences */
    setenv("SYS$LOGIN",   rec->default_dir, 1);
    setenv("SYS$SCRATCH", "/tmp",           1);

    /* Initialize user PCB (lives until exec replaces address space) */
    uint64_t user_privs = parse_privilege_string(rec->privileges);
    struct vms_pcb *pcb = vms_pcb_init(user_privs);
    if (pcb) {
        uint32_t uic = (rec->uic_group << 16) | rec->uic_member;
        char prcnam[16];
        snprintf(prcnam, sizeof(prcnam), "_FTA%d:", (int)(getpid() % 100));
        vms_pcb_set_identity((uint32_t)getpid(), uic, rec->username, prcnam);
        vms_pcb_set_default_dir(rec->default_dir);
    }

    /* Exec the DCL shell with --login flag */
    execl(DCL_SHELL_PATH, "vmsdcl", "--login", (char *)NULL);

    /* If exec fails, fall back to sh */
    perror("vmsdcl");
    fprintf(stderr, "Falling back to /bin/sh\n");
    execl("/bin/sh", "sh", (char *)NULL);
    _exit(1);
}

/* ------------------------------------------------------------------ */
/* SSH mode: user already authenticated by PAM against sysuaf.dat     */
/* ------------------------------------------------------------------ */
static int ssh_login(void)
{
    const char *user = getenv("USER");
    if (!user || user[0] == '\0')
        return 1;

    char username[64];
    strncpy(username, user, sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';
    upcase(username);

    sysuaf_record_t user_rec;
    memset(&user_rec, 0, sizeof(user_rec));
    if (lookup_user(username, &user_rec) < 0) {
        fprintf(stderr, "User authorization failure\n");
        return 1;
    }

    start_session(&user_rec);
    return 1;  /* Should not reach here */
}

/* ------------------------------------------------------------------ */
/* Console mode: interactive login with username/password prompts      */
/* ------------------------------------------------------------------ */
static int console_login(void)
{
    char username[64];
    char password[128];
    sysuaf_record_t user_rec;
    int attempts = 0;

    /* Disable stdio buffering on stdin so that unread data remains
     * in the kernel pipe/tty buffer and is available after exec(). */
    setvbuf(stdin, NULL, _IONBF, 0);

    while (attempts < MAX_ATTEMPTS) {
        /* Prompt for username */
        printf("Username: ");
        fflush(stdout);
        if (fgets(username, sizeof(username), stdin) == NULL)
            return 1;  /* EOF */
        trim_trailing(username);
        upcase(username);

        if (username[0] == '\0')
            continue;

        /* Prompt for password */
        printf("Password: ");
        fflush(stdout);
        if (read_password(password, sizeof(password)) < 0)
            return 1;

        /* Look up user */
        memset(&user_rec, 0, sizeof(user_rec));
        if (lookup_user(username, &user_rec) < 0) {
            printf("\nUser authorization failure\n\n");
            attempts++;
            continue;
        }

        /* Authenticate */
        if (!authenticate(&user_rec, password)) {
            printf("\nUser authorization failure\n\n");
            attempts++;
            continue;
        }

        /* --- Authentication successful --- */
        start_session(&user_rec);
        return 1;  /* Should not reach here */
    }

    printf("\nMaximum login attempts exceeded. Disconnecting.\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Main: dispatch based on --ssh flag                                  */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "--ssh") == 0)
        return ssh_login();
    return console_login();
}
