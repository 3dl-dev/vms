/*
 * mksysuaf.c - generate a binary $UAFDEF SYSUAF.DAT (vms-d92, epic vms-d0c).
 *
 * The atomic-flip SEED. Writes SYS$SYSTEM:SYSUAF.DAT as a genuine RMS Prolog-3
 * INDEXED file of 644-byte $UAFDEF records (primary USERNAME key + secondary
 * UIC key), with passwords stored as the real VMS Purdy hash (UAI$C_PURDY_S).
 * This REPLACES the shipped ASCII+SHA-256 SYSUAF.DAT: the distro no longer ships
 * an ASCII authorization file.
 *
 * Runs on the BUILD host (executive absent): it writes the indexed image to a
 * plain filesystem path (argv[1]) through rms_io_posix_wrap + the sysuaf_rms
 * engine -- the SAME bytes the runtime reads back over the ACP (the record
 * format is substrate-agnostic, vms-5f0). No ACP, no /dev/vms needed to author
 * the seed.
 *
 * Usage:  mksysuaf <output-path>
 *
 * The default account set MIRRORS the accounts the ASCII SYSUAF shipped:
 *   SYSTEM   [1,4]   ALL                      password MANAGER
 *   OPERATOR [1,6]   OPER,SYSPRV,TMPMBX,NETMBX  (no password -> cannot log in)
 *   DEFAULT  [128,128] TMPMBX,NETMBX            (no password -> template acct)
 *   GUEST    [128,129] TMPMBX                  password GUEST
 *   USER1    [128,130] TMPMBX,NETMBX            (no password)
 *   USER2    [128,131] TMPMBX,NETMBX            (no password)
 * UICs are the numeric values the ASCII file expressed in OCTAL (vms-e60):
 * [200,200] octal == [128,128] decimal, etc. An account with no password gets
 * no PURDY_S credential, so sysuaf_authenticate refuses every password -- the
 * binary equivalent of the ASCII "empty hash => cannot authenticate" rule.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "sysuaf.h"        /* view + view_to_raw + set_password (libvms)      */
#include "sysuaf_rms.h"    /* the binary indexed-file engine (vmsrms)         */
#include "rms_io.h"        /* rms_io_posix_wrap / _unwrap                      */
#include "rmsdef.h"
#include "ssdef.h"

struct seed_acct {
    const char *user;
    uint16_t    grp, mem;
    const char *dir;
    const char *flags;      /* UAI flag names (empty for none)                 */
    const char *priv;       /* privilege names, or "ALL"                       */
    const char *pw;         /* plaintext password, or NULL for "cannot log in" */
};

static const struct seed_acct g_seed[] = {
    { "SYSTEM",   1,   4,   "SYS$SYSDEVICE:[SYSMGR]",        "", "ALL",                     "MANAGER" },
    { "OPERATOR", 1,   6,   "SYS$SYSDEVICE:[SYSMGR]",        "", "OPER,SYSPRV,TMPMBX,NETMBX", NULL     },
    { "DEFAULT",  128, 128, "SYS$SYSDEVICE:[USERS.DEFAULT]", "", "TMPMBX,NETMBX",           NULL     },
    { "GUEST",    128, 129, "SYS$SYSDEVICE:[USERS.GUEST]",   "", "TMPMBX",                  "GUEST"  },
    { "USER1",    128, 130, "SYS$SYSDEVICE:[USERS.USER1]",   "", "TMPMBX,NETMBX",           NULL     },
    { "USER2",    128, 131, "SYS$SYSDEVICE:[USERS.USER2]",   "", "TMPMBX,NETMBX",           NULL     },
};

/* Build one $UAFDEF record from a seed row. */
static void build_record(const struct seed_acct *a, sysuaf_rms_record_t *out)
{
    sysuaf_record_t v;
    memset(&v, 0, sizeof(v));
    strncpy(v.username, a->user, sizeof(v.username) - 1);
    v.uic_group  = a->grp;
    v.uic_member = a->mem;
    strncpy(v.default_dir, a->dir, sizeof(v.default_dir) - 1);
    strncpy(v.flags, a->flags, sizeof(v.flags) - 1);
    /* "ALL" is not a single privilege name; map it to the full mask below. */
    if (strcmp(a->priv, "ALL") != 0)
        strncpy(v.privileges, a->priv, sizeof(v.privileges) - 1);

    sysuaf_view_to_raw(&v);

    if (strcmp(a->priv, "ALL") == 0) {
        /* SYSTEM/ALL: every privilege bit set (authorized + default). */
        memset(v.raw.uaf$q_priv, 0xff, sizeof(v.raw.uaf$q_priv));
        memset(v.raw.uaf$q_def_priv, 0xff, sizeof(v.raw.uaf$q_def_priv));
    }

    if (a->pw) {
        /* DETERMINISTIC salt (reproducible build): a fixed function of the
         * username, NOT a random word -- so the shipped SYSUAF is byte-for-byte
         * identical across builds (reproducibility, vms-d73). The salt is stored
         * with the record and reused for verification, so any value authenticates
         * correctly; determinism is the only reason it is not random here. */
        uint16_t salt = 0;
        for (const char *p = a->user; *p; p++)
            salt = (uint16_t)(salt * 31u + (unsigned char)*p);
        sysuaf_set_password_salt(&v, a->pw, salt);
    }
    /* else: no PURDY_S credential -> uaf$b_encrypt stays 0 -> refuses login. */

    *out = v.raw;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-path>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        perror("mksysuaf: open");
        return 1;
    }

    rms_file_t *h = rms_io_posix_wrap(fd);
    if (!h) {
        fprintf(stderr, "mksysuaf: rms_io_posix_wrap failed\n");
        close(fd);
        return 1;
    }

    sysuaf_rms_file_t sf;
    uint32_t st = sysuaf_rms_create(h, &sf);
    if (st != RMS$_CREATED) {
        fprintf(stderr, "mksysuaf: sysuaf_rms_create failed (0x%08x)\n", st);
        rms_io_posix_unwrap(h);
        return 1;
    }

    unsigned n = (unsigned)(sizeof(g_seed) / sizeof(g_seed[0]));
    for (unsigned i = 0; i < n; i++) {
        sysuaf_rms_record_t rec;
        build_record(&g_seed[i], &rec);
        st = sysuaf_put_record(&sf, &rec);
        if (!$VMS_STATUS_SUCCESS(st)) {
            fprintf(stderr, "mksysuaf: put %s failed (0x%08x)\n",
                    g_seed[i].user, st);
            sysuaf_rms_close(&sf);
            rms_io_posix_unwrap(h);
            return 1;
        }
    }

    sysuaf_rms_close(&sf);
    rms_io_posix_unwrap(h);
    printf("mksysuaf: wrote binary $UAFDEF SYSUAF (%u accounts) to %s\n",
           n, argv[1]);
    return 0;
}
