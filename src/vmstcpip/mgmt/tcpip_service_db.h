#ifndef TCPIP_SERVICE_DB_H
#define TCPIP_SERVICE_DB_H
/*
 * tcpip_service_db.h - TCP/IP Services for OVMX: the PERSISTENT INETD SERVICE
 * DATABASE management engine (vms-67f rung, #878).
 *
 * WHAT THIS IS. The auxiliary server TCPIP$INETD (src/vmstcpip/services/
 * tcpip_inetd.h) binds and serves every ENABLED service named in
 * SYS$SYSTEM:TCPIP$SERVICE.DAT, which it reads over the Files-11 ACP at
 * startup. That database was already PERSISTENT (an ODS-2-resident file the
 * boot @SYS$STARTUP:TCPIP$STARTUP starts the aux server against) -- but the
 * ONLY way to add/enable a service was to hand-edit the text file. This header
 * is the missing MANAGEMENT plane: the engine the DCL `TCPIP {SET,SHOW,ENABLE,
 * DISABLE,DELETE} SERVICE` verbs drive to edit that database THE VMS WAY, with
 * the change persisted over the ACP so it survives reboot and is reapplied by
 * the aux server on the next TCPIP$INETD start.
 *
 * FAITHFULNESS. Real TCPIP$INETD is configured by `TCPIP SET SERVICE ... ;
 * ENABLE SERVICE ...` which write the internal TCPIP$SERVICE store, read back
 * at aux-server start. VSI's binary store layout is not published, so the
 * on-disk LINE FORMAT is the SAME OVMX design choice tcpip_inetd.h already
 * documents (Rule 8) -- this engine reads and REWRITES exactly those records,
 * so the bytes this plane persists are the bytes the real consumer parses. A
 * service DEFINED but not enabled is kept as a leading-"!" record (the disabled
 * convention the aux-server parser already skips); ENABLE strips the "!",
 * DISABLE restores it -- the database carries the record either way, exactly as
 * VMS keeps a defined-but-disabled service.
 *
 * INV-6 (CLAUDE.md Rule 9). Persistence rides RMS over the Files-11 ACP
 * (rms_textfile_*, the vms-274 writer idiom). The INV-6-critical operation is
 * the persistent WRITE: with no executive / no mounted ACP volume the store
 * returns -1 -- an honest failure the DCL layer reports as %TCPIP-W-NOEXEC,
 * NEVER a per-process table that reports success while persisting nothing. No
 * fabrication: a genuine ACP write or an honest error.
 *
 * Proven against a real /dev/vms by tests/qemu/test_syssvc_tcpip_service_db.c:
 * a service SET+ENABLEd through this engine is read back through the INDEPENDENT
 * aux-server parser (tcpip_inetd_parse_db) reading the persisted file, DISABLE
 * removes it from what the aux server would bind, and DELETE drops it -- the
 * exact persistence the plane exists to provide, anchored by the negctl defect
 * 'tcpip-svcdb-enable-not-persisted'.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>            /* strcasecmp */
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

#include "rms_textfile.h"       /* rms_textfile_* -- RMS text I/O over the ACP */

/* The one VMS filespec of the service database (resolved through LNM$FILE_DEV,
 * honouring DEFINE/SYSTEM SYS$SYSTEM). This is the SAME file TCPIP$INETD reads
 * at aux-server start and @SYS$STARTUP:TCPIP$STARTUP names. */
#define TCPIP_SVCDB_SPEC        "SYS$SYSTEM:TCPIP$SERVICE.DAT"

#define TCPIP_SVCDB_MAX          16     /* matches TCPIP_INETD_MAX_SERVICES */
#define TCPIP_SVCDB_NAME_MAX     32
#define TCPIP_SVCDB_PATH_MAX     256
#define TCPIP_SVCDB_ARGS_MAX     256

/* enable_flag values for tcpip_svcdb_set(). */
#define TCPIP_SVC_DISABLE   0
#define TCPIP_SVC_ENABLE    1
#define TCPIP_SVC_KEEP    (-1)   /* preserve the existing enabled state (new => disabled) */

/* Return codes shared by the mutators. */
#define TCPIP_SVCDB_OK        0
#define TCPIP_SVCDB_ENOEXEC (-1)   /* ACP write failed -- no executive/volume (INV-6 honest) */
#define TCPIP_SVCDB_EFULL   (-2)   /* database already holds TCPIP_SVCDB_MAX services */
#define TCPIP_SVCDB_ENOSUCH (-3)   /* named service not defined */
#define TCPIP_SVCDB_EINVAL  (-4)   /* bad argument */

struct tcpip_svcdb_rec {
    char     name[TCPIP_SVCDB_NAME_MAX];
    uint16_t port;
    char     user[TCPIP_SVCDB_NAME_MAX];   /* run-as SYSUAF account (R4 G1); "" = none */
    char     image[TCPIP_SVCDB_PATH_MAX];  /* service image filespec (argv[0]) */
    char     args[TCPIP_SVCDB_ARGS_MAX];   /* whitespace-separated extra argv */
    int      enabled;                      /* 1 = uncommented (aux server binds it); 0 = "!"-disabled */
};

/* Parse ONE raw SERVICE.DAT line into `r`. Returns 1 if the line is a service
 * record (r->enabled reflects whether it was "!"-disabled), 0 if it is a blank
 * or a pure banner comment (a "!" line whose remainder does not parse as
 * "name port ..."). The record grammar is byte-identical to the aux-server
 * parser (tcpip_inetd_parse_db): name port [user] image [args], where the token
 * after port is a run-as USERNAME iff it carries neither ':' nor '/'. */
static inline int tcpip_svcdb_parse_rec(const char *raw, struct tcpip_svcdb_rec *r)
{
    char line[512];
    char *q, *tok, *save = NULL;
    long port;
    int disabled = 0;

    if (!raw || !r) return 0;
    strncpy(line, raw, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    q = line;
    while (*q && isspace((unsigned char)*q)) q++;
    if (*q == '!') {                       /* a defined-but-disabled record, or a banner */
        disabled = 1;
        q++;
        while (*q && isspace((unsigned char)*q)) q++;
    }
    if (*q == '\0')                        /* blank line, or a bare "!" */
        return 0;

    memset(r, 0, sizeof(*r));

    tok = strtok_r(q, " \t", &save);       /* service-name */
    if (!tok) return 0;
    strncpy(r->name, tok, sizeof(r->name) - 1);

    tok = strtok_r(NULL, " \t", &save);    /* port */
    if (!tok) return 0;
    errno = 0;
    port = strtol(tok, NULL, 10);
    if (errno != 0 || port <= 0 || port > 65535)
        return 0;                          /* not a service record => banner comment */
    r->port = (uint16_t)port;

    tok = strtok_r(NULL, " \t", &save);    /* run-as USERNAME or image-filespec */
    if (!tok) return 0;
    if (strchr(tok, ':') == NULL && strchr(tok, '/') == NULL) {
        strncpy(r->user, tok, sizeof(r->user) - 1);
        tok = strtok_r(NULL, " \t", &save);/* image-filespec */
        if (!tok) return 0;
    }
    strncpy(r->image, tok, sizeof(r->image) - 1);

    tok = strtok_r(NULL, "", &save);       /* remainder = args */
    if (tok) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        strncpy(r->args, tok, sizeof(r->args) - 1);
    }

    r->enabled = !disabled;
    return 1;
}

/* Render `r` to its SERVICE.DAT line (a leading "! " iff disabled). */
static inline void tcpip_svcdb_format_rec(const struct tcpip_svcdb_rec *r,
                                          char *buf, size_t sz)
{
    char body[TCPIP_SVCDB_NAME_MAX + TCPIP_SVCDB_NAME_MAX +
              TCPIP_SVCDB_PATH_MAX + TCPIP_SVCDB_ARGS_MAX + 32];
    if (r->user[0])
        snprintf(body, sizeof(body), "%s %u %s %s%s%s",
                 r->name, (unsigned)r->port, r->user, r->image,
                 r->args[0] ? " " : "", r->args);
    else
        snprintf(body, sizeof(body), "%s %u %s%s%s",
                 r->name, (unsigned)r->port, r->image,
                 r->args[0] ? " " : "", r->args);
    snprintf(buf, sz, "%s%s", r->enabled ? "" : "! ", body);
}

/* Load ALL records (enabled and "!"-disabled) from the service database `spec`
 * over the ACP into recs[0..max-1]. Returns the count (>= 0). A database that
 * cannot be opened (absent file, or no executive) yields 0 records -- an ABSENT
 * database is a legitimate empty starting point for a first SET SERVICE; the
 * INV-6 honesty is enforced on the persistent WRITE (tcpip_svcdb_store_at),
 * which $CREATEs the file and fails honestly with no ACP.
 *
 * The `_at` forms take the database spec explicitly; the production wrappers
 * below default it to TCPIP_SVCDB_SPEC. The test drives `_at` against a
 * writable fixture spec so it exercises this SAME engine byte-exact (the SPEC
 * ISOLATION rationale of test_syssvc_tcpip_config_acp.c). */
static inline int tcpip_svcdb_load_at(const char *spec,
                                      struct tcpip_svcdb_rec *recs, int max)
{
    rms_textfile_t *tf;
    char rec[512];
    int n = 0;

    if (!spec || !recs || max <= 0)
        return 0;
    tf = rms_textfile_open(spec);
    if (!tf)
        return 0;
    while (n < max && rms_textfile_getline(tf, rec, sizeof(rec), NULL) == 1) {
        if (tcpip_svcdb_parse_rec(rec, &recs[n]) == 1)
            n++;
    }
    rms_textfile_close(tf);
    return n;
}

/* Return the index of `name` (case-insensitive) in recs[0..n-1], or -1. */
static inline int tcpip_svcdb_find(const struct tcpip_svcdb_rec *recs, int n,
                                   const char *name)
{
    if (!recs || !name) return -1;
    for (int i = 0; i < n; i++)
        if (strcasecmp(recs[i].name, name) == 0)
            return i;
    return -1;
}

/* Rewrite the whole database `spec` over the ACP, superseding its prior
 * contents: a deterministic generated header (write_line -- the guaranteed
 * first write) then one appended record per service. Returns TCPIP_SVCDB_OK, or
 * TCPIP_SVCDB_ENOEXEC if the ACP write fails (no executive / no volume). */
static inline int tcpip_svcdb_store_at(const char *spec,
                                       const struct tcpip_svcdb_rec *recs, int n)
{
    char line[768];

    if (!spec)
        return TCPIP_SVCDB_EINVAL;
    if (rms_textfile_write_line(spec,
            "! TCPIP$SERVICE.DAT -- managed by TCPIP {SET,ENABLE,DISABLE,DELETE} SERVICE (vms-71b).") != 0)
        return TCPIP_SVCDB_ENOEXEC;
    for (int i = 0; i < n; i++) {
        tcpip_svcdb_format_rec(&recs[i], line, sizeof(line));
        if (rms_textfile_append_line(spec, line) != 0)
            return TCPIP_SVCDB_ENOEXEC;
    }
    return TCPIP_SVCDB_OK;
}

/* Define or update the service `name` (upsert) in `spec` and persist. A NEW
 * service defaults DISABLED (operator posture) unless enable_flag ==
 * TCPIP_SVC_ENABLE; an EXISTING service keeps its enabled state when
 * enable_flag == TCPIP_SVC_KEEP, else is set to enable_flag. `user`/`args` may
 * be NULL (=> ""). */
static inline int tcpip_svcdb_set_at(const char *spec, const char *name,
                                     uint16_t port, const char *user,
                                     const char *image, const char *args,
                                     int enable_flag)
{
    struct tcpip_svcdb_rec recs[TCPIP_SVCDB_MAX];
    int n, i;

    if (!spec || !name || !name[0] || !image || !image[0] || port == 0)
        return TCPIP_SVCDB_EINVAL;

    n = tcpip_svcdb_load_at(spec, recs, TCPIP_SVCDB_MAX);
    i = tcpip_svcdb_find(recs, n, name);
    if (i < 0) {
        if (n >= TCPIP_SVCDB_MAX)
            return TCPIP_SVCDB_EFULL;
        i = n++;
        memset(&recs[i], 0, sizeof(recs[i]));
        strncpy(recs[i].name, name, sizeof(recs[i].name) - 1);
        recs[i].enabled = (enable_flag == TCPIP_SVC_ENABLE) ? 1 : 0; /* NEGCTL tcpip-svcdb-enable-not-persisted */
    } else if (enable_flag != TCPIP_SVC_KEEP) {
        recs[i].enabled = (enable_flag == TCPIP_SVC_ENABLE) ? 1 : 0;
    }
    recs[i].port = port;
    recs[i].user[0] = '\0';
    if (user) strncpy(recs[i].user, user, sizeof(recs[i].user) - 1);
    recs[i].image[0] = '\0';
    strncpy(recs[i].image, image, sizeof(recs[i].image) - 1);
    recs[i].args[0] = '\0';
    if (args) strncpy(recs[i].args, args, sizeof(recs[i].args) - 1);

    return tcpip_svcdb_store_at(spec, recs, n);
}

/* Toggle the enabled state of a DEFINED service in `spec` and persist. Returns
 * TCPIP_SVCDB_ENOSUCH if the service is not defined. */
static inline int tcpip_svcdb_enable_at(const char *spec, const char *name, int on)
{
    struct tcpip_svcdb_rec recs[TCPIP_SVCDB_MAX];
    int n, i;

    if (!spec || !name || !name[0])
        return TCPIP_SVCDB_EINVAL;
    n = tcpip_svcdb_load_at(spec, recs, TCPIP_SVCDB_MAX);
    i = tcpip_svcdb_find(recs, n, name);
    if (i < 0)
        return TCPIP_SVCDB_ENOSUCH;
    recs[i].enabled = on ? 1 : 0;
    return tcpip_svcdb_store_at(spec, recs, n);
}

/* Remove a DEFINED service from `spec` and persist. TCPIP_SVCDB_ENOSUCH if absent. */
static inline int tcpip_svcdb_delete_at(const char *spec, const char *name)
{
    struct tcpip_svcdb_rec recs[TCPIP_SVCDB_MAX];
    int n, i;

    if (!spec || !name || !name[0])
        return TCPIP_SVCDB_EINVAL;
    n = tcpip_svcdb_load_at(spec, recs, TCPIP_SVCDB_MAX);
    i = tcpip_svcdb_find(recs, n, name);
    if (i < 0)
        return TCPIP_SVCDB_ENOSUCH;
    for (int j = i; j < n - 1; j++)
        recs[j] = recs[j + 1];
    n--;
    return tcpip_svcdb_store_at(spec, recs, n);
}

/* ---- Production wrappers: the DCL verbs drive the real SYS$SYSTEM: database -- */
static inline int tcpip_svcdb_load(struct tcpip_svcdb_rec *recs, int max)
{ return tcpip_svcdb_load_at(TCPIP_SVCDB_SPEC, recs, max); }

static inline int tcpip_svcdb_set(const char *name, uint16_t port,
                                  const char *user, const char *image,
                                  const char *args, int enable_flag)
{ return tcpip_svcdb_set_at(TCPIP_SVCDB_SPEC, name, port, user, image, args, enable_flag); }

static inline int tcpip_svcdb_enable(const char *name, int on)
{ return tcpip_svcdb_enable_at(TCPIP_SVCDB_SPEC, name, on); }

static inline int tcpip_svcdb_delete(const char *name)
{ return tcpip_svcdb_delete_at(TCPIP_SVCDB_SPEC, name); }

#endif /* TCPIP_SERVICE_DB_H */
