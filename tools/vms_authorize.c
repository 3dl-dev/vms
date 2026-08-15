/*
 * vms_authorize.c - VMS AUTHORIZE utility for SYSUAF management
 *
 * Implements the OpenVMS AUTHORIZE utility interface for managing
 * the System User Authorization File (SYSUAF) at /etc/ovmx/sysuaf.dat
 *
 * SHA256 is provided by libvms (src/libvms/rtl/sha256.c).
 * Build: part of tools/ CMakeLists.txt (links vms library)
 *
 * Feature-test macros (_POSIX_C_SOURCE/_DEFAULT_SOURCE on Linux, or
 * _NETBSD_SOURCE on NetBSD -- flock()/LOCK_EX in <sys/file.h> are a BSD
 * extension gated OFF by _POSIX_C_SOURCE on NetBSD, vms-64a) are supplied
 * by tools/CMakeLists.txt's target_compile_definitions(vms_authorize ...),
 * not hardcoded here, so the netbsd-vax cross build can select the wider
 * namespace -- same pattern src/ovmx_init/CMakeLists.txt uses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

#include "sha256.h"
#include "sysuaf.h"
#include "str_util.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "vms/logical.h"
#include "ovmx_identity.h"
/* This process's privilege mask comes from the executive (vms-b2e). */
#include "prvdef.h"
#include "vms_kif.h"
#define MAX_USERS     1024
#define MAX_LINE      1024
/* AUTHORIZE reports the OVMX product version from the SSOT (INV-1). */
#define UAF_VERSION   OVMX_PRODUCT_VERSION

/* Default values for new users.
 * UIC numbers are OCTAL (vms-e60), so this literal is written 0200 -- it is
 * the group [200,...] a VMS administrator reads, not decimal 200, which
 * would display as [310,...]. */
#define DEFAULT_UIC_GROUP    0200
#define DEFAULT_PRIVS        "TMPMBX,NETMBX"

/* In-memory database */
static sysuaf_record_t g_users[MAX_USERS];
static int             g_nusers = 0;
static int             g_dirty  = 0;   /* set when in-memory data modified */
static int             g_load_errors = 0; /* records refused at load time */

/* str_upcase() and str_trim() replaced by str_str_upcase()/str_trim() from str_util.h */

/* Skip leading whitespace; return pointer into s */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* ------------------------------------------------------------------ */
/* VMS minimum-abbreviation command matching                           */
/* ------------------------------------------------------------------ */

/*
 * Returns 1 if 'input' is a valid minimum abbreviation of 'full'.
 * Input must be at least min_len characters and a prefix of full.
 * Case-insensitive.
 */
static int matches_abbrev(const char *input, const char *full, size_t min_len)
{
    size_t ilen = strlen(input);
    size_t flen = strlen(full);

    if (ilen < min_len || ilen > flen)
        return 0;

    return (strncasecmp(input, full, ilen) == 0);
}

/* ------------------------------------------------------------------ */
/* SYSUAF file I/O                                                     */
/* ------------------------------------------------------------------ */

/* Load all users from SYSUAF_PATH into g_users[] */
static int load_sysuaf(void)
{
    g_nusers = 0;

    char sysuaf_linux[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux, sizeof(sysuaf_linux));
    FILE *fp = fopen(sysuaf_linux, "r");
    if (!fp) {
        /* Non-fatal if file doesn't exist yet */
        if (errno == ENOENT)
            return 0;
        fprintf(stderr, "%%UAF-E-OPENIN, error opening %s: %s\n",
                sysuaf_linux, strerror(errno));
        return -1;
    }

    /* ONE READER (vms-9b7): sysuaf_read_line() + sysuaf_parse_line() from
     * libvms. AUTHORIZE used to carry its own 1024-byte fgets loop and its
     * own field split, one of five independent implementations of this
     * format with three different buffer sizes between them. */
    char line[SYSUAF_LINE_MAX];
    int lineno = 0;
    int too_long = 0;
    while (sysuaf_read_line(fp, line, sizeof(line), &too_long)) {
        lineno++;

        /* A record that does not fit is REFUSED, not silently shortened.
         * AUTHORIZE must not load a row it cannot faithfully write back:
         * saving the truncated form would destroy the operator's data. */
        if (too_long) {
            fprintf(stderr, "%%UAF-E-RECTOOLONG, record at line %d exceeds "
                    "%d bytes -- not loaded\n", lineno, SYSUAF_LINE_MAX - 1);
            g_load_errors++;
            continue;
        }

        if (g_nusers >= MAX_USERS) {
            fprintf(stderr, "%%UAF-W-OVERFLOW, too many users (max %d), "
                    "line %d ignored\n", MAX_USERS, lineno);
            continue;
        }

        int rc = sysuaf_parse_line(line, &g_users[g_nusers]);
        if (rc == 0)
            continue;   /* comment or blank */
        if (rc < 0) {
            fprintf(stderr, "%%UAF-W-BADFORMAT, malformed record at line %d\n",
                    lineno);
            continue;
        }

        g_nusers++;
    }

    fclose(fp);
    return 0;
}

/* Write all users back to SYSUAF_PATH, with flock() for safety */
static int save_sysuaf(void)
{
    /*
     * REFUSE TO SAVE OVER RECORDS WE COULD NOT LOAD. save_sysuaf() rewrites
     * the WHOLE file from g_users[], so if load_sysuaf() dropped a row --
     * because it was longer than the record limit and reading its prefix
     * would have corrupted it -- committing this save would DELETE that
     * account. Losing an account silently is the same class of defect as
     * losing the SYSTEM record silently, which is what this item exists to
     * kill (vms-9b7).
     */
    if (g_load_errors) {
        fprintf(stderr, "%%UAF-E-SAVEABORT, %d record(s) could not be read at "
                "load time; refusing to rewrite SYSUAF and lose them\n",
                g_load_errors);
        return -1;
    }

    /* Write to a temp file then rename for atomicity */
    char sysuaf_linux2[1024];
    vmsfs_to_linux_path(SYSUAF_PATH, sysuaf_linux2, sizeof(sysuaf_linux2));
    char tmppath[1024];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp.%d", sysuaf_linux2, (int)getpid());

    FILE *fp = fopen(tmppath, "w");
    if (!fp) {
        fprintf(stderr, "%%UAF-E-OPENOUT, error creating %s: %s\n",
                tmppath, strerror(errno));
        return -1;
    }

    /* Lock the temp file during write */
    if (flock(fileno(fp), LOCK_EX) != 0) {
        fprintf(stderr, "%%UAF-W-LOCKFAIL, could not lock file: %s\n",
                strerror(errno));
        /* Proceed anyway — best effort */
    }

    fprintf(fp, "# System User Authorization File\n");
    fprintf(fp, "# Format: USERNAME|PASSWORD_HASH|UIC_GROUP|UIC_MEMBER|"
                "DEFAULT_DIR|FLAGS|PRIVILEGES\n");
    fprintf(fp, "# Password hash is SHA256 hex. Empty = account CANNOT AUTHENTICATE\n");
    fprintf(fp, "# (no password, right or wrong, is accepted) -- see vms-08f and the\n");
    fprintf(fp, "# disposition comment on sysuaf_authenticate() in src/libvms/rtl/sysuaf.c.\n");

    /*
     * ONE WRITER (vms-9b7). The format string lived here AND in
     * src/libvms/syssvc/sys_uai.c, and the two disagreed on the FLAGS field
     * ("%s" here, "%u" there). It now lives in exactly one place --
     * sysuaf_format_record() -- and that function REFUSES a record too long
     * to be read back, rather than emitting one that a reader will silently
     * truncate. Refusing is the whole point: a row this process cannot read
     * back is a row that can leave the system with no SYSTEM record and no
     * boot (measured, vms-9b7).
     */
    int refused = 0;
    for (int i = 0; i < g_nusers; i++) {
        const sysuaf_record_t *r = &g_users[i];
        char row[SYSUAF_LINE_MAX];
        if (sysuaf_format_record(r, row, sizeof(row)) < 0) {
            fprintf(stderr, "%%UAF-E-RECTOOLONG, record for %s exceeds the "
                    "%d-byte SYSUAF record limit -- NOT WRITTEN\n",
                    r->username, SYSUAF_LINE_MAX - 1);
            refused++;
            continue;
        }
        fprintf(fp, "%s\n", row);
    }

    fflush(fp);
    flock(fileno(fp), LOCK_UN);
    fclose(fp);

    /* A refused record means the file on disk would not be what the operator
     * asked for. Abandon the whole save rather than commit a partial one. */
    if (refused) {
        fprintf(stderr, "%%UAF-E-SAVEABORT, %d record(s) refused; SYSUAF not "
                "updated\n", refused);
        unlink(tmppath);
        return -1;
    }

    /* Atomic rename */
    if (rename(tmppath, sysuaf_linux2) != 0) {
        fprintf(stderr, "%%UAF-E-RENAMEFAIL, error saving %s: %s\n",
                sysuaf_linux2, strerror(errno));
        unlink(tmppath);
        return -1;
    }

    g_dirty = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* User lookup                                                         */
/* ------------------------------------------------------------------ */

/* Find index of user by name; returns -1 if not found */
static int find_user(const char *username)
{
    char uname[64];
    strncpy(uname, username, sizeof(uname) - 1);
    uname[sizeof(uname) - 1] = '\0';
    str_upcase(uname);

    for (int i = 0; i < g_nusers; i++) {
        if (strcmp(g_users[i].username, uname) == 0)
            return i;
    }
    return -1;
}

/* Find highest UIC member number for a given group */
static uint32_t next_uic_member(uint32_t group)
{
    /* 0200 OCTAL (vms-e60): the floor must be in the same base as the member
     * numbers it is compared against, or it sorts above every real account.
     * With the shipped SYSUAF (members 201/202/203 octal = 129/130/131) a
     * decimal 200 floor would beat all of them and hand out 0311. */
    uint32_t max_member = 0200;
    for (int i = 0; i < g_nusers; i++) {
        if (g_users[i].uic_group == group &&
            g_users[i].uic_member > max_member)
            max_member = g_users[i].uic_member;
    }
    return max_member + 1;
}

/* ------------------------------------------------------------------ */
/* Qualifier parsing helpers                                           */
/* ------------------------------------------------------------------ */

/*
 * Parse /QUALIFIER=value or /QUALIFIER=(v1,v2,...) from cmdline.
 * Returns pointer to the value string (without outer parens if list),
 * or NULL if qualifier not present.
 * The returned pointer is into a static buffer — valid until next call.
 */
static const char *find_qualifier(const char *line, const char *qual_name)
{
    static char result[512];

    /* Build search key: /QUALNAME= or /QUALNAME (flag-only) */
    char key[64];
    snprintf(key, sizeof(key), "/%s", qual_name);

    /* Case-insensitive search */
    const char *p = line;
    size_t klen = strlen(key);
    while (*p) {
        if (strncasecmp(p, key, klen) == 0) {
            const char *after = p + klen;
            if (*after == '=') {
                after++;
                /* Collect value: parenthesized list or single token */
                if (*after == '(') {
                    after++;
                    const char *end = strchr(after, ')');
                    if (!end) end = after + strlen(after);
                    size_t len = (size_t)(end - after);
                    if (len >= sizeof(result)) len = sizeof(result) - 1;
                    strncpy(result, after, len);
                    result[len] = '\0';
                } else {
                    /* Read until space or / or end */
                    const char *end = after;
                    while (*end && *end != ' ' && *end != '\t' && *end != '/')
                        end++;
                    size_t len = (size_t)(end - after);
                    if (len >= sizeof(result)) len = sizeof(result) - 1;
                    strncpy(result, after, len);
                    result[len] = '\0';
                }
                return result;
            } else if (*after == '\0' || *after == ' ' || *after == '\t' ||
                       *after == '/') {
                /* Flag-only qualifier with no value */
                result[0] = '\0';
                return result;
            }
        }
        p++;
    }
    return NULL;  /* Qualifier not present */
}

/* Hash a password string to SHA256 hex; returns empty string for empty pw */
static void hash_password(const char *pw, char hex[128])
{
    if (!pw || pw[0] == '\0') {
        hex[0] = '\0';
        return;
    }
    sha256_hex((const uint8_t *)pw, strlen(pw), hex);
}

/* ------------------------------------------------------------------ */
/* UAF> Commands                                                       */
/* ------------------------------------------------------------------ */

/* ADD username [qualifiers] */
static void cmd_add(const char *args)
{
    /* Extract username (first token) */
    const char *p = skip_ws(args);
    if (!p || *p == '\0') {
        printf("%%UAF-E-SYNTAX, username required\n");
        return;
    }

    char username[64];
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '/' && i < 63)
        username[i++] = *p++;
    username[i] = '\0';
    str_upcase(username);

    if (username[0] == '\0') {
        printf("%%UAF-E-SYNTAX, username required\n");
        return;
    }

    /* Check for duplicate */
    if (find_user(username) >= 0) {
        printf("%%UAF-E-USEREXISTS, user %s already exists\n", username);
        return;
    }

    if (g_nusers >= MAX_USERS) {
        printf("%%UAF-E-OVERFLOW, user database is full\n");
        return;
    }

    /* Build new record with defaults */
    sysuaf_record_t rec;
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.username, username, sizeof(rec.username) - 1);
    rec.uic_group  = DEFAULT_UIC_GROUP;
    rec.uic_member = next_uic_member(DEFAULT_UIC_GROUP);
    snprintf(rec.default_dir, sizeof(rec.default_dir),
             "SYS$SYSDEVICE:[USERS.%s]", username);
    strncpy(rec.privileges, DEFAULT_PRIVS, sizeof(rec.privileges) - 1);

    /* Parse qualifiers from rest of line */
    const char *val;

    val = find_qualifier(args, "PASSWORD");
    if (val)
        hash_password(val, rec.password_hash);

    val = find_qualifier(args, "UIC");
    if (val) {
        /* Format: [group,member] or group,member */
        const char *v = val;
        if (*v == '[') v++;
        unsigned g = 0, m = 0;
        /* /UIC=[g,m] is typed in octal (vms-e60) */
        if (sscanf(v, "%o,%o", &g, &m) == 2) {
            rec.uic_group  = g;
            rec.uic_member = m;
        }
    }

    /* DEVICE qualifier: sets the device portion of the default directory.
     * Default is SYS$SYSDEVICE. */
    const char *device = "SYS$SYSDEVICE";
    val = find_qualifier(args, "DEVICE");
    if (val)
        device = val;

    val = find_qualifier(args, "DIRECTORY");
    if (val) {
        /* Accept [DIR.SUBDIR] or DIR.SUBDIR format */
        const char *v = val;
        if (*v == '[') v++;
        char dir[256];
        strncpy(dir, v, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        size_t dlen = strlen(dir);
        if (dlen > 0 && dir[dlen - 1] == ']')
            dir[dlen - 1] = '\0';
        snprintf(rec.default_dir, sizeof(rec.default_dir),
                 "%s:[%s]", device, dir);
    }

    val = find_qualifier(args, "PRIVILEGES");
    if (val)
        strncpy(rec.privileges, val, sizeof(rec.privileges) - 1);

    val = find_qualifier(args, "FLAGS");
    if (val)
        strncpy(rec.flags, val, sizeof(rec.flags) - 1);

    /* Append to database */
    g_users[g_nusers++] = rec;
    g_dirty = 1;

    printf("%%UAF-S-ADDMSG, user record successfully added\n");
}

/* MODIFY username [qualifiers] */
static void cmd_modify(const char *args)
{
    const char *p = skip_ws(args);
    if (!p || *p == '\0') {
        printf("%%UAF-E-SYNTAX, username required\n");
        return;
    }

    char username[64];
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '/' && i < 63)
        username[i++] = *p++;
    username[i] = '\0';
    str_upcase(username);

    int idx = find_user(username);
    if (idx < 0) {
        printf("%%UAF-E-NOSUCHUSER, no such user %s\n", username);
        return;
    }

    sysuaf_record_t *r = &g_users[idx];
    const char *val;

    val = find_qualifier(args, "PASSWORD");
    if (val)
        hash_password(val, r->password_hash);

    val = find_qualifier(args, "FLAGS");
    if (val) {
        /* Append or replace flags */
        strncpy(r->flags, val, sizeof(r->flags) - 1);
        r->flags[sizeof(r->flags) - 1] = '\0';
    }

    val = find_qualifier(args, "PRIVILEGES");
    if (val) {
        strncpy(r->privileges, val, sizeof(r->privileges) - 1);
        r->privileges[sizeof(r->privileges) - 1] = '\0';
    }

    val = find_qualifier(args, "DEFDIR");
    if (val) {
        /* Accept full VMS spec (DEV:[DIR]) or just [DIR] or DIR */
        if (strchr(val, ':') || (val[0] == '[' && strchr(val, ']'))) {
            /* Full VMS spec — store as-is */
            strncpy(r->default_dir, val, sizeof(r->default_dir) - 1);
            r->default_dir[sizeof(r->default_dir) - 1] = '\0';
        } else {
            const char *v = val;
            if (*v == '[') v++;
            char dir[256];
            strncpy(dir, v, sizeof(dir) - 1);
            dir[sizeof(dir) - 1] = '\0';
            size_t dlen = strlen(dir);
            if (dlen > 0 && dir[dlen - 1] == ']')
                dir[dlen - 1] = '\0';
            snprintf(r->default_dir, sizeof(r->default_dir),
                     "SYS$SYSDEVICE:[%s]", dir);
        }
    }

    val = find_qualifier(args, "UIC");
    if (val) {
        const char *v = val;
        if (*v == '[') v++;
        unsigned g = 0, m = 0;
        /* /UIC=[g,m] is typed in octal (vms-e60) */
        if (sscanf(v, "%o,%o", &g, &m) == 2) {
            r->uic_group  = (uint32_t)g;
            r->uic_member = (uint32_t)m;
        }
    }

    g_dirty = 1;
    printf("%%UAF-S-MDFYMSG, user record(s) updated\n");
}

/* REMOVE username */
static void cmd_remove(const char *args)
{
    const char *p = skip_ws(args);
    if (!p || *p == '\0') {
        printf("%%UAF-E-SYNTAX, username required\n");
        return;
    }

    char username[64];
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '/' && i < 63)
        username[i++] = *p++;
    username[i] = '\0';
    str_upcase(username);

    int idx = find_user(username);
    if (idx < 0) {
        printf("%%UAF-E-NOSUCHUSER, no such user %s\n", username);
        return;
    }

    /* Prompt for confirmation */
    printf("Remove user %s from SYSUAF? [Y/N]: ", username);
    fflush(stdout);
    char answer[16];
    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        printf("\n%%UAF-W-ABORTED, operation cancelled\n");
        return;
    }
    str_trim(answer);
    str_upcase(answer);
    if (answer[0] != 'Y') {
        printf("%%UAF-W-ABORTED, operation cancelled\n");
        return;
    }

    /* Shift remaining records down */
    for (int j = idx; j < g_nusers - 1; j++)
        g_users[j] = g_users[j + 1];
    g_nusers--;
    g_dirty = 1;

    printf("%%UAF-S-REMMSG, user record removed\n");
}

/* LIST [/FULL] */
static void cmd_list(const char *args)
{
    int full = (find_qualifier(args, "FULL") != NULL);
    (void)full;  /* /FULL shows same info for now (full detail via SHOW) */

    printf("\n");
    printf("%-18s %-12s %-22s %-10s %s\n",
           "Username", "UIC", "Default Directory", "Flags", "Privileges");
    printf("%-18s %-12s %-22s %-10s %s\n",
           "--------", "---", "-----------------", "-----", "----------");

    for (int i = 0; i < g_nusers; i++) {
        const sysuaf_record_t *r = &g_users[i];
        char uic_str[16];
        /* octal, as VMS writes UICs (vms-e60) */
        snprintf(uic_str, sizeof(uic_str), "[%03o,%03o]",
                 r->uic_group, r->uic_member);
        printf("%-18s %-12s %-22s %-10s %s\n",
               r->username,
               uic_str,
               r->default_dir,
               r->flags,
               r->privileges);
    }
    printf("\n");
}

/* SHOW username */
static void cmd_show(const char *args)
{
    const char *p = skip_ws(args);
    if (!p || *p == '\0') {
        /* SHOW with no args: show all (same as LIST) */
        cmd_list("");
        return;
    }

    char username[64];
    int i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '/' && i < 63)
        username[i++] = *p++;
    username[i] = '\0';
    str_upcase(username);

    int idx = find_user(username);
    if (idx < 0) {
        printf("%%UAF-E-NOSUCHUSER, no such user %s\n", username);
        return;
    }

    const sysuaf_record_t *r = &g_users[idx];
    char uic_str[16];
    /* octal, as VMS writes UICs (vms-e60) */
    snprintf(uic_str, sizeof(uic_str), "[%03o,%03o]",
             r->uic_group, r->uic_member);

    printf("\n");
    printf("Username: %-30s Owner:\n", r->username);
    printf("Account:  %-30s UIC:    %s\n", "", uic_str);
    printf("Default:  %s\n", r->default_dir);
    printf("Flags:    %s\n", r->flags[0] ? r->flags : "(none)");
    printf("Primary days: Mon Tue Wed Thu Fri\n");
    printf("Privileges:\n");
    if (r->privileges[0])
        printf("  %s\n", r->privileges);
    else
        printf("  (none)\n");
    printf("Password hash: %s\n",
           r->password_hash[0] ? r->password_hash
                                : "(none — account cannot authenticate)");
    printf("\n");
}

/* HELP */
static void cmd_help(void)
{
    printf("\n");
    printf("AUTHORIZE commands:\n");
    printf("\n");
    printf("  ADD username /PASSWORD=xxx /UIC=[grp,mem] /DEVICE=disk\n");
    printf("               /DIRECTORY=[dir] /PRIVILEGES=(list) /FLAGS=(list)\n");
    printf("      Create a new SYSUAF entry.\n");
    printf("\n");
    printf("  MODIFY username /PASSWORD=xxx /FLAGS=(list)\n");
    printf("                  /PRIVILEGES=(list) /DEFDIR=[dir] /UIC=[grp,mem]\n");
    printf("      Update fields of an existing SYSUAF entry.\n");
    printf("      /FLAGS=DISUSER disables the account.\n");
    printf("\n");
    printf("  REMOVE username\n");
    printf("      Remove an entry from SYSUAF (prompts for confirmation).\n");
    printf("\n");
    printf("  LIST [/FULL]\n");
    printf("      Display all accounts in a formatted table.\n");
    printf("\n");
    printf("  SHOW username\n");
    printf("      Display detailed information for one account.\n");
    printf("\n");
    printf("  HELP\n");
    printf("      Display this help text.\n");
    printf("\n");
    printf("  EXIT\n");
    printf("      Save changes and exit AUTHORIZE.\n");
    printf("\n");
    printf("All commands accept minimum abbreviations (e.g., AD=ADD, MO=MODIFY,\n");
    printf("RE=REMOVE, LI=LIST, SH=SHOW, HE=HELP, EX=EXIT).\n");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Privilege check                                                     */
/* ------------------------------------------------------------------ */

/*
 * Does this process hold SYSPRV?
 *
 * ============================================================
 * THE ANSWER COMES FROM THE EXECUTIVE (vms-b2e)
 * ============================================================
 * AUTHORIZE manages SYSUAF. On VMS the right to do that is decided by the
 * privilege mask the executive holds for the process -- never by anything
 * the process is able to say about itself.
 *
 * WHAT USED TO STAND HERE, so nobody puts it back:
 *
 *     if (geteuid() == 0) return 1;
 *     const char *login = getenv("USER");   -- upcased, looked up in
 *                                              SYSUAF, ALL/SYSPRV granted
 *
 * MEASURED on a host build of main a4ead89, against the built binary with
 * the shipped SYSUAF: `env USER=baron` was refused with %UAF-F-NOAUTH,
 * while `env USER=SYSTEM` opened a full SYSUAF-management session and
 * printed SYSTEM's password hash. One environment variable flipped
 * refusal into total control of the authorization database.
 *
 * Both of the old tests were HOST facts. geteuid() is the Linux uid this
 * binary happens to run under; getenv("USER") is a string the caller
 * chooses. Neither is a VMS identity. Consulting the host environment for
 * a VMS privilege decision is the illegal third answer CLAUDE.md Rule 10
 * names: it had the shape of an access control system and the substance
 * of an honor system.
 *
 * The executive derives the UIC from credentials a process cannot grant
 * itself, and a user name arrives only through VMS_IOCTL_SETIDENT, which
 * refuses any caller without SETPRV an identity that is not a weakening
 * of its own. So cur_privs is what something privileged established for
 * this process, which is the difference between an identity and a claim
 * (Rule 11).
 *
 * THERE IS NO ABSENT-EXECUTIVE BRANCH AND MUST NOT BE ONE (Rule 9). PID 1
 * refuses to bring OVMX up without /dev/vms, so on the one OVMX runtime
 * this read cannot fail. If it fails anyway -- a developer running this
 * binary on a bare Linux host -- AUTHORIZE refuses, because a process
 * whose privileges nothing can vouch for holds none. Substituting a local
 * guess for a failed executive read is this same defect wearing a
 * different name.
 *
 * SYSPRV SUBSUMES THE OLD "ALL" TEST: a mask carrying every privilege has
 * bit PRV$V_SYSPRV set, so the two-word check is not lost by dropping to
 * one bit. PRV$M_SYSPRV is held to the executive's own VMS_PRV_M_SYSPRV
 * by _Static_assert in src/libvms/prv_agreement.c, so this is a pinned
 * constant and not a self-certified one (Rule 8).
 * ============================================================
 */
static int check_privilege(void)
{
    struct vms_procinfo self;
    memset(&self, 0, sizeof(self));

    if (!(vms_kif_getjpi_self(&self) & 1))
        return 0;

    return (self.cur_privs & PRV$M_SYSPRV) != 0;
}

/* ------------------------------------------------------------------ */
/* Main REPL                                                           */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Bootstrap VMS namespace */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    /* Privilege check */
    if (!check_privilege()) {
        /*
         * ORACLE-GROUNDED MESSAGE (vms-4d7). The old text
         *   "%UAF-F-NOAUTH, insufficient privilege to manage SYSUAF"
         * was INVENTED -- no such AUTHORIZE facility message exists on VMS,
         * and %UAF-F-NOAUTH is not a real code. Captured on the reference
         * lab (lab-2 replica vaxlab-7, OpenVMS VAX V7.3, node VAX2,
         * 8-AUG-2026) by running AUTHORIZE from an account WITHOUT SYSPRV
         * (user TESTNP, UIC [200,200], privileges TMPMBX+NETMBX) that is not
         * the owner of SYS$SYSTEM:SYSUAF.DAT ([SYSTEM], protection RWE,RWE,,):
         *
         *   %UAF-E-NAOFIL, unable to open system authorization file (SYSUAF.DAT)
         *   -RMS-E-PRV, insufficient privilege or file protection violation
         *
         * On VMS AUTHORIZE does not pre-check a privilege bit; it opens
         * SYSUAF and RMS refuses a non-privileged non-owner. OVMX pre-checks
         * SYSPRV (check_privilege() above) but must report what VMS reports.
         * Capture and method: docs/oracle/vax73-authorize-privilege.md.
         */
        fprintf(stderr,
            "%%UAF-E-NAOFIL, unable to open system authorization file (SYSUAF.DAT)\n");
        fprintf(stderr,
            "-RMS-E-PRV, insufficient privilege or file protection violation\n");
        return 1;
    }

    /* Print banner */
    printf("%%UAF-I-AUTHVERSION, AUTHORIZE %s\n", UAF_VERSION);

    /* Load existing SYSUAF */
    if (load_sysuaf() < 0)
        return 1;

    /* Interactive command loop */
    char line[MAX_LINE];
    for (;;) {
        printf("UAF> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF — treat as EXIT */
            printf("\n");
            break;
        }

        str_trim(line);
        const char *p = skip_ws(line);
        if (!p || *p == '\0')
            continue;

        /* Extract command verb (up to first space or /) */
        char verb[32];
        int vi = 0;
        while (*p && *p != ' ' && *p != '\t' && vi < 31)
            verb[vi++] = *p++;
        verb[vi] = '\0';
        str_upcase(verb);

        /* Skip whitespace after verb */
        while (*p == ' ' || *p == '\t')
            p++;

        /* Dispatch on minimum abbreviation */
        if (matches_abbrev(verb, "ADD", 2)) {
            cmd_add(p);
        } else if (matches_abbrev(verb, "MODIFY", 2)) {
            cmd_modify(p);
        } else if (matches_abbrev(verb, "REMOVE", 2)) {
            cmd_remove(p);
        } else if (matches_abbrev(verb, "LIST", 2)) {
            cmd_list(p);
        } else if (matches_abbrev(verb, "SHOW", 2)) {
            cmd_show(p);
        } else if (matches_abbrev(verb, "HELP", 2)) {
            cmd_help();
        } else if (matches_abbrev(verb, "EXIT", 2)) {
            break;
        } else {
            printf("%%UAF-E-BADCOMMAND, unrecognized command '%s' -- type HELP for available commands\n",
                   verb);
        }
    }

    /* Save on exit if modified */
    if (g_dirty) {
        if (save_sysuaf() == 0)
            printf("%%UAF-I-SAVED, SYSUAF written successfully\n");
        else
            printf("%%UAF-E-SAVEFAIL, error writing SYSUAF\n");
    }

    return 0;
}
