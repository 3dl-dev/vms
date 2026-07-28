/*
 * cluster_authorize.h - VMScluster group authorization (vms-ci.8)
 *
 * On real VMS, the cluster group number and password are NOT SYSGEN
 * parameters — they live in a separate small authorization record
 * (VMS calls the on-disk artifact CLUSTER_AUTHORIZE.DAT, written by
 * CLUSTER_CONFIG.COM). This header defines a MINIMAL OVMX stand-in
 * with the same separation: a tiny typed file, distinct from
 * sysgen_params.h, holding just the group number and password.
 *
 * Scope (vms-ci.8): configurable + readable only. No hashing, no
 * wire use yet — the password field is stored in the clear for now.
 * Hashing and wire authentication are later rungs (vms-ci.5).
 */
#ifndef CLUSTER_AUTHORIZE_H
#define CLUSTER_AUTHORIZE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLUSTER_AUTH_MAGIC        0x43415554u  /* "CAUT" */
#define CLUSTER_AUTH_VERSION      1
#define CLUSTER_AUTH_DEFAULT_PATH "/etc/ovmx/cluster_authorize.dat"
#define CLUSTER_AUTH_PWD_LEN      32

struct cluster_authorize {
    uint32_t    magic;
    uint32_t    version;
    uint16_t    group;                          /* cluster group number */
    char        password[CLUSTER_AUTH_PWD_LEN]; /* plaintext for now; see vms-ci.5 */
};

/*
 * cluster_authorize_path - Resolve the path to the cluster authorization
 * file. Honors OVMX_CLUSTER_AUTH_PATH for tests/tools, mirroring
 * sysgen_db_path() in sysgen_params.h.
 */
static inline const char *cluster_authorize_path(void)
{
    const char *p = getenv("OVMX_CLUSTER_AUTH_PATH");
    return (p && *p) ? p : CLUSTER_AUTH_DEFAULT_PATH;
}

/*
 * cluster_authorize_read - Read the cluster group/password record.
 * Returns 0 on success, -1 on error (file missing, bad magic/version).
 */
static inline int cluster_authorize_read(struct cluster_authorize *out)
{
    if (!out) return -1;

    FILE *fp = fopen(cluster_authorize_path(), "rb");
    if (!fp) return -1;

    struct cluster_authorize rec;
    if (fread(&rec, sizeof(rec), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (rec.magic != CLUSTER_AUTH_MAGIC || rec.version != CLUSTER_AUTH_VERSION)
        return -1;

    *out = rec;
    return 0;
}

/*
 * cluster_authorize_write - Write the cluster group/password record.
 * Returns 0 on success, -1 on error (open/write failure).
 */
static inline int cluster_authorize_write(uint16_t group, const char *password)
{
    struct cluster_authorize rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic   = CLUSTER_AUTH_MAGIC;
    rec.version = CLUSTER_AUTH_VERSION;
    rec.group   = group;
    if (password) {
        strncpy(rec.password, password, CLUSTER_AUTH_PWD_LEN - 1);
    }

    FILE *fp = fopen(cluster_authorize_path(), "wb");
    if (!fp) return -1;

    size_t n = fwrite(&rec, sizeof(rec), 1, fp);
    fclose(fp);
    return (n == 1) ? 0 : -1;
}

#endif /* CLUSTER_AUTHORIZE_H */
