/*
 * test_sysgen_identity.c - Node identity + cluster SYSGEN params (vms-ci.8)
 *
 * Verifies:
 *   (a) the v2 typed SYSGEN format round-trips a string param (SCSNODE)
 *       and a numeric param (SCSSYSTEMID) through sysgen_read_string()/
 *       sysgen_read_param() — the runtime readers, which were dead code
 *       before this item.
 *   (c) sys$getsyi(SYI$_SCSNODE) / sys$getsyi(SYI$_SCSSYSTEMID) — via the
 *       lib$getsyi wrapper, which calls sys$getsyiw -> sys$getsyi, the
 *       same pattern used by the existing test_lib_getsyi() coverage in
 *       test_lib_rtl.c — return the configured values, not the hostname.
 *
 * Uses OVMX_SYSGEN_PATH to point the readers at a private temp file so
 * the test needs no write access to /etc/ovmx (not writable by a
 * non-root CI/dev user; see sysgen_db_path() in sysgen_params.h).
 *
 * F$GETSYI("SCSNODE") vs. F$GETSYI("NODENAME") divergence (b) is proven
 * at the DCL layer by tests/dcl/test_lexical_scsnode.sh.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "prcdef.h"
#include "sysgen_params.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* Build a minimal v2 SYSGEN database with one string param (SCSNODE) and
 * one numeric param (SCSSYSTEMID), and write it to `path`. */
static void write_test_db(const char *path)
{
    struct sysgen_file db;
    memset(&db, 0, sizeof(db));
    db.magic   = SYSGEN_MAGIC;
    db.version = SYSGEN_VERSION;
    db.count   = 2;

    struct sysgen_param *scsnode = &db.params[0];
    strncpy(scsnode->name, "SCSNODE", sizeof(scsnode->name) - 1);
    scsnode->type = SYSGEN_TYPE_STRING;
    strncpy(scsnode->str_current, "TESTND", sizeof(scsnode->str_current) - 1);
    strncpy(scsnode->str_default, "OVMX", sizeof(scsnode->str_default) - 1);

    struct sysgen_param *sysid = &db.params[1];
    strncpy(sysid->name, "SCSSYSTEMID", sizeof(sysid->name) - 1);
    sysid->type = SYSGEN_TYPE_NUMERIC;
    sysid->current = 4242;
    sysid->default_val = 0;
    sysid->min_val = 0;
    sysid->max_val = 65535;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "test_sysgen_identity: cannot open %s for write\n", path);
        exit(1);
    }
    if (fwrite(&db, sizeof(db), 1, fp) != 1) {
        fprintf(stderr, "test_sysgen_identity: write failed\n");
        exit(1);
    }
    fclose(fp);
}

/* --- (a) raw reader round-trip --- */
static void test_reader_round_trip(void)
{
    printf("Testing sysgen_read_string/sysgen_read_param round-trip...\n");

    char node[SYSGEN_STRVAL_LEN];
    int rc = sysgen_read_string("SCSNODE", node, sizeof(node));
    check(rc == 0, "sysgen_read_string(SCSNODE) succeeds");
    check(strcmp(node, "TESTND") == 0, "sysgen_read_string(SCSNODE) == TESTND");

    /* Case-insensitive lookup, matches sysgen_read_param's existing convention */
    rc = sysgen_read_string("scsnode", node, sizeof(node));
    check(rc == 0 && strcmp(node, "TESTND") == 0,
          "sysgen_read_string is case-insensitive");

    uint32_t sysid = 0;
    rc = sysgen_read_param("SCSSYSTEMID", &sysid);
    check(rc == 0, "sysgen_read_param(SCSSYSTEMID) succeeds");
    check(sysid == 4242, "sysgen_read_param(SCSSYSTEMID) == 4242");

    /* Type mismatch: reading a string param numerically (or vice versa) fails */
    uint32_t bogus = 0;
    rc = sysgen_read_param("SCSNODE", &bogus);
    check(rc != 0, "sysgen_read_param rejects a string-typed parameter");

    char bogus_str[SYSGEN_STRVAL_LEN];
    rc = sysgen_read_string("SCSSYSTEMID", bogus_str, sizeof(bogus_str));
    check(rc != 0, "sysgen_read_string rejects a numeric-typed parameter");

    /* Unknown parameter */
    rc = sysgen_read_param("NOSUCHPARAM", &bogus);
    check(rc != 0, "sysgen_read_param fails for an unknown parameter");
}

/* --- (c) sys$getsyi via lib$getsyi wrapper --- */
static void test_getsyi_identity(void)
{
    printf("Testing sys$getsyi(SYI$_SCSNODE / SYI$_SCSSYSTEMID)...\n");

    char nbuf[SYSGEN_STRVAL_LEN];
    memset(nbuf, 0, sizeof(nbuf));
    struct dsc$descriptor_s ndesc;
    ndesc.dsc$w_length = sizeof(nbuf) - 1;
    ndesc.dsc$b_dtype = DSC$K_DTYPE_T;
    ndesc.dsc$b_class = DSC$K_CLASS_S;
    ndesc.dsc$a_pointer = nbuf;
    uint16_t nlen = 0;

    uint32_t item = SYI$_SCSNODE;
    uint32_t st = lib$getsyi(&item, NULL, &ndesc, &nlen, NULL, NULL);
    check(st == SS$_NORMAL, "lib$getsyi(SYI$_SCSNODE) returns SS$_NORMAL");
    nbuf[nlen < sizeof(nbuf) ? nlen : sizeof(nbuf) - 1] = '\0';
    check(strcmp(nbuf, "TESTND") == 0,
          "lib$getsyi(SYI$_SCSNODE) returns the configured node name");

    uint32_t sysid_result = 0;
    item = SYI$_SCSSYSTEMID;
    st = lib$getsyi(&item, &sysid_result, NULL, NULL, NULL, NULL);
    check(st == SS$_NORMAL, "lib$getsyi(SYI$_SCSSYSTEMID) returns SS$_NORMAL");
    check(sysid_result == 4242,
          "lib$getsyi(SYI$_SCSSYSTEMID) returns the configured system ID");

    /* SYI$_NODENAME must still be the Linux hostname, not SCSNODE — proves
     * the two identities stay distinct at the sys$getsyi layer too. */
    char hostbuf[256];
    memset(hostbuf, 0, sizeof(hostbuf));
    struct dsc$descriptor_s hdesc;
    hdesc.dsc$w_length = sizeof(hostbuf) - 1;
    hdesc.dsc$b_dtype = DSC$K_DTYPE_T;
    hdesc.dsc$b_class = DSC$K_CLASS_S;
    hdesc.dsc$a_pointer = hostbuf;
    uint16_t hlen = 0;
    item = SYI$_NODENAME;
    st = lib$getsyi(&item, NULL, &hdesc, &hlen, NULL, NULL);
    check(st == SS$_NORMAL, "lib$getsyi(SYI$_NODENAME) returns SS$_NORMAL");
    hostbuf[hlen < sizeof(hostbuf) ? hlen : sizeof(hostbuf) - 1] = '\0';
    check(strcmp(hostbuf, "TESTND") != 0,
          "SYI$_NODENAME (hostname) differs from the configured SCSNODE");
}

int main(void)
{
    printf("test_sysgen_identity: node identity + cluster SYSGEN params (vms-ci.8)\n");

    char path[] = "/tmp/ovmx_test_sysgen_identity_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "test_sysgen_identity: mkstemp failed\n");
        return 1;
    }
    close(fd);

    write_test_db(path);
    setenv("OVMX_SYSGEN_PATH", path, 1);

    test_reader_round_trip();
    test_getsyi_identity();

    unlink(path);

    printf("test_sysgen_identity: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
