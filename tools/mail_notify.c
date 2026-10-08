/*
 * mail_notify.c - the OVMX MAIL store's READ side: where a user's mail file is,
 * how its records replay into messages, and the login-time new-mail count.
 *
 * ONE IMPLEMENTATION (vms-417, rd vms-47fd). Compiled into MAIL.EXE,
 * MAIL_SERVER.EXE and LOGINOUT, so the file MAIL reads, the file the DECnet
 * MAIL-11 server delivers into and the file the login notice counts are one
 * file, reached one way: rms_textfile_* (RMS $OPEN/$GET over the executive
 * ACP). See vms_mail_notify.h for the record format.
 *
 * DELETED, NOT REPLACED: the POSIX ~/.vmsmail directory (fopen of
 * "<default dir>/.vmsmail/MAIL.IDX") and build_maildir()'s "/home/<user>"
 * fallback. Neither was a VMS file; a network delivery written there would have
 * been a side file no VMS facility could see.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vms_mail_notify.h"
#include "rms_textfile.h"
/* SYSUAF is the sole VMS account database — binary indexed lookup (vms-d92). */
#include "sysuaf.h"

int get_user_homedir(const char *username, char *homedir, size_t sz)
{
    sysuaf_record_t rec;
    if (sysuaf_lookup(username, &rec) == 0) {
        strncpy(homedir, rec.default_dir, sz - 1);
        homedir[sz - 1] = '\0';
        memset(&rec, 0, sizeof rec);     /* the record carries the hash */
        return 0;
    }
    return -1;
}

int mail_store_spec(const char *username, char *spec, size_t sz)
{
    char dir[512];
    if (!username || !*username || get_user_homedir(username, dir, sizeof dir) != 0 ||
        dir[0] == '\0')
        return -1;
    int n = snprintf(spec, sz, "%s%s", dir, MAIL_STORE_FILE);
    return (n > 0 && (size_t)n < sz) ? 0 : -1;
}

/* Split "X|<16 hex>|payload". Returns the tag, or 0 for a record to skip. */
static char split_rec(char *rec, char **id, char **payload)
{
    if (rec[0] == '\0' || rec[1] != '|') return 0;
    char *p = rec + 2;
    for (int i = 0; i < MAIL_ID_LEN; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
    }
    if (p[MAIL_ID_LEN] == '|') { p[MAIL_ID_LEN] = '\0'; *payload = p + MAIL_ID_LEN + 1; }
    else if (p[MAIL_ID_LEN] == '\0') *payload = p + MAIL_ID_LEN;
    else return 0;
    *id = p;
    return rec[0];
}

static void setf(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

int mail_store_load(const char *username, struct mail_store_entry *out, int max,
                    int *count)
{
    char spec[600];
    *count = 0;
    if (mail_store_spec(username, spec, sizeof spec) != 0) return -1;
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) return 0;                       /* no mail file: no mail */

    /* Every message seen, committed or not; `committed` filters at the end. */
    static unsigned char committed[MAIL_STORE_MAX];
    int n = 0;
    char rec[1024];
    int too_long = 0;
    while (rms_textfile_getline(tf, rec, sizeof rec, &too_long)) {
        size_t rl = strlen(rec);
        if (rl && rec[rl - 1] == '\n') rec[--rl] = '\0';
        if (too_long) continue;
        char *id = NULL, *pl = NULL;
        char tag = split_rec(rec, &id, &pl);
        if (!tag) continue;
        int k = -1;
        for (int i = n - 1; i >= 0; i--)
            if (strcmp(out[i].id, id) == 0) { k = i; break; }
        if (tag == 'H') {
            if (k >= 0 || n >= max || n >= MAIL_STORE_MAX) continue;
            k = n++;
            memset(&out[k], 0, sizeof out[k]);
            committed[k] = 0;
            setf(out[k].id, sizeof out[k].id, id);
            setf(out[k].date, sizeof out[k].date, pl);
            continue;
        }
        if (k < 0) continue;
        switch (tag) {
        case 'F': setf(out[k].from, sizeof out[k].from, pl); break;
        case 'T': setf(out[k].to,   sizeof out[k].to,   pl); break;
        case 'C': setf(out[k].cc,   sizeof out[k].cc,   pl); break;
        case 'S': setf(out[k].subj, sizeof out[k].subj, pl); break;
        case 'E': committed[k] = 1; break;
        case 'R': out[k].read = 1; break;
        case 'D': out[k].deleted = 1; break;
        default: break;
        }
    }
    rms_textfile_close(tf);

    /* Drop what never committed, keeping order. */
    int w = 0;
    for (int i = 0; i < n; i++)
        if (committed[i]) { if (w != i) out[w] = out[i]; w++; }
    *count = w;
    return 0;
}

int mail_store_body(const char *username, const char *id,
                    void (*line_cb)(void *ctx, const char *line), void *ctx)
{
    char spec[600];
    if (mail_store_spec(username, spec, sizeof spec) != 0) return -1;
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) return -1;
    int lines = 0;
    char rec[1024];
    int too_long = 0;
    while (rms_textfile_getline(tf, rec, sizeof rec, &too_long)) {
        size_t rl = strlen(rec);
        if (rl && rec[rl - 1] == '\n') rec[--rl] = '\0';
        char *rid = NULL, *pl = NULL;
        if (too_long || split_rec(rec, &rid, &pl) != 'B' || strcmp(rid, id) != 0)
            continue;
        line_cb(ctx, pl);
        lines++;
    }
    rms_textfile_close(tf);
    return lines;
}

int mail_count_unread(const char *username)
{
    /* A compact replay (LOGINOUT links this): id + state per message only. */
    static struct { char id[MAIL_ID_LEN + 1]; unsigned char st; } m[MAIL_STORE_MAX];
    enum { M_COMMIT = 1, M_READ = 2, M_DEL = 4 };
    char spec[600];
    if (mail_store_spec(username, spec, sizeof spec) != 0) return 0;
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) return 0;
    int n = 0;
    char rec[1024];
    int too_long = 0;
    while (rms_textfile_getline(tf, rec, sizeof rec, &too_long)) {
        size_t rl = strlen(rec);
        if (rl && rec[rl - 1] == '\n') rec[--rl] = '\0';
        char *id = NULL, *pl = NULL;
        char tag = too_long ? 0 : split_rec(rec, &id, &pl);
        if (tag != 'H' && tag != 'E' && tag != 'R' && tag != 'D') continue;
        int k = -1;
        for (int i = n - 1; i >= 0; i--)
            if (strcmp(m[i].id, id) == 0) { k = i; break; }
        if (tag == 'H') {
            if (k < 0 && n < MAIL_STORE_MAX) {
                setf(m[n].id, sizeof m[n].id, id);
                m[n++].st = 0;
            }
            continue;
        }
        if (k < 0) continue;
        m[k].st |= (tag == 'E') ? M_COMMIT : (tag == 'R') ? M_READ : M_DEL;
    }
    rms_textfile_close(tf);
    int unread = 0;
    for (int i = 0; i < n; i++)
        if (m[i].st == M_COMMIT) unread++;
    return unread;
}
