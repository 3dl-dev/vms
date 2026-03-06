/*
 * sysgen_params.h - VMS SYSGEN parameter database definitions
 *
 * Defines the binary format for the system parameter database used by
 * SYSGEN.EXE, and provides a reader function for other OVMX components
 * to query current parameter values.
 */
#ifndef SYSGEN_PARAMS_H
#define SYSGEN_PARAMS_H

#include <stdint.h>

#define SYSGEN_MAGIC        0x53595347  /* "SYSG" */
#define SYSGEN_VERSION      1
#define SYSGEN_MAX_PARAMS   64
#define SYSGEN_DEFAULT_PATH "/etc/ovmx/sysparams.dat"

/* Parameter flags */
#define SYSGEN_F_DYNAMIC        0x01    /* Can be changed without reboot */
#define SYSGEN_F_INFORMATIONAL  0x02    /* Read-only informational parameter */

struct sysgen_param {
    char        name[32];
    uint32_t    current;
    uint32_t    default_val;
    uint32_t    min_val;
    uint32_t    max_val;
    uint8_t     flags;          /* SYSGEN_F_DYNAMIC, SYSGEN_F_INFORMATIONAL */
    char        description[80];
};

struct sysgen_file {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    count;
    struct sysgen_param params[SYSGEN_MAX_PARAMS];
};

/*
 * sysgen_read_param - Read a single parameter value from the current database
 *
 * Reads /etc/ovmx/sysparams.dat and returns the current value for the
 * named parameter.
 *
 * Returns 0 on success, -1 on error (file not found, parameter not found).
 */
static inline int sysgen_read_param(const char *name, uint32_t *value)
{
    if (!name || !value) return -1;

    FILE *fp = fopen(SYSGEN_DEFAULT_PATH, "rb");
    if (!fp) return -1;

    struct sysgen_file db;
    if (fread(&db, sizeof(db), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (db.magic != SYSGEN_MAGIC || db.version != SYSGEN_VERSION)
        return -1;

    for (uint32_t i = 0; i < db.count && i < SYSGEN_MAX_PARAMS; i++) {
        if (strncasecmp(db.params[i].name, name, 32) == 0) {
            *value = db.params[i].current;
            return 0;
        }
    }
    return -1;
}

#endif /* SYSGEN_PARAMS_H */
