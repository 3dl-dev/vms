/*
 * test_dnet_ncpstore.c - the DECnet configuration STORE NCP.EXE and NETACP share
 * (rd vms-1f69, src/vmsdecnet/ncp/dnet_ncpstore.c).
 *
 * Proves, on the build host:
 *   1. The databases are named by their VMS file specifications --
 *      SYS$SYSTEM:NETNODE_LOCAL.DAT / NETNODE_REMOTE.DAT / NETOBJECT.DAT -- and
 *      with no host override that IS where they live (dnet_store_location).
 *   2. FAIL HONEST with no file layer: this test links LIBVMS but NOT LIBVMSRMS,
 *      so the RMS services behind rms_textfile_* are absent -- exactly the
 *      "no executive / no ACP volume" condition. A load then reads as an empty
 *      database and every SAVE fails DNET_STORE_EWRITE; nothing is written to a
 *      Linux path instead (the pre-vms-1f69 /etc/ovmx/decnet default is gone).
 *   3. The host-test hook (OVMX_DECNET_* env) round-trips all three databases,
 *      each file opening with the comment that labels the layout as OVMX's (not
 *      the VMS binary format), and a corrupt executor record yields NO address.
 * The booted-runtime proof (NCP SET EXECUTOR lands in SYS$SYSTEM:NETNODE_LOCAL.DAT
 * through RMS over the ACP and NETACP self-sources it) is the DECnet section of
 * tests/qemu/lib/dcl_acceptance_battery.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "dnet_ncpstore.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else { printf("  FAIL: %s\n", m); fail++; } } while (0)

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static int file_has(const char *path, const char *needle)
{
    char buf[4096];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

int main(void)
{
    printf("=== test_dnet_ncpstore ===\n");
    unsetenv("OVMX_DECNET_EXECUTOR");
    unsetenv("OVMX_DECNET_NODEDB");
    unsetenv("OVMX_DECNET_OBJECTDB");

    /* ---- 1. the VMS file specifications -------------------------------- */
    CHECK(strcmp(dnet_store_vms_spec(DNET_STORE_EXECUTOR), "SYS$SYSTEM:NETNODE_LOCAL.DAT") == 0,
          "executor database is SYS$SYSTEM:NETNODE_LOCAL.DAT");
    CHECK(strcmp(dnet_store_vms_spec(DNET_STORE_NODES), "SYS$SYSTEM:NETNODE_REMOTE.DAT") == 0,
          "node database is SYS$SYSTEM:NETNODE_REMOTE.DAT");
    CHECK(strcmp(dnet_store_vms_spec(DNET_STORE_OBJECTS), "SYS$SYSTEM:NETOBJECT.DAT") == 0,
          "object database is SYS$SYSTEM:NETOBJECT.DAT");
    CHECK(dnet_store_host_override(DNET_STORE_EXECUTOR) == NULL &&
          strcmp(dnet_store_location(DNET_STORE_EXECUTOR), "SYS$SYSTEM:NETNODE_LOCAL.DAT") == 0,
          "with no host override the executor lives at the VMS file (no Linux default path)");

    /* ---- 2. fail honest with no file layer ------------------------------ */
    {
        int etc_before = exists("/etc/ovmx/decnet/executor.dat");
        struct dnet_executor x;
        memset(&x, 0xA5, sizeof(x));
        CHECK(dnet_store_load_executor(&x) == DNET_STORE_OK && !x.have_addr,
              "no file layer: the executor database reads as ABSENT (unconfigured), never invented");
        x.have_addr = 1; x.addr = (1u << 10) | 42u; strcpy(x.name, "OVMX"); x.state_on = 1;
        CHECK(dnet_store_save_executor(&x) == DNET_STORE_EWRITE,
              "no file layer: SAVE EXECUTOR fails DNET_STORE_EWRITE (honest), not a silent success");
        struct dnet_nodedb nd;
        dnet_nodedb_init(&nd);
        (void)dnet_nodedb_set(&nd, (1u << 10) | 1u, "VAX1");
        CHECK(dnet_store_save_nodes(&nd) == DNET_STORE_EWRITE,
              "no file layer: SAVE NODES fails honestly");
        struct dnet_objectdb od;
        dnet_objectdb_init(&od);
        (void)dnet_objectdb_set(&od, 17, "FAL", "");
        CHECK(dnet_store_save_objects(&od) == DNET_STORE_EWRITE,
              "no file layer: SAVE OBJECTS fails honestly");
        CHECK(exists("/etc/ovmx/decnet/executor.dat") == etc_before,
              "nothing was written to the retired /etc/ovmx/decnet Linux path");
    }

    /* ---- 3. the host-test hook round-trips ------------------------------ */
    char dir[] = "/tmp/ncpstoreXXXXXX";
    if (!mkdtemp(dir)) { printf("  FAIL: mkdtemp\n"); return 1; }
    char pe[256], pn[256], po[256];
    snprintf(pe, sizeof(pe), "%s/e.dat", dir);
    snprintf(pn, sizeof(pn), "%s/n.dat", dir);
    snprintf(po, sizeof(po), "%s/o.dat", dir);
    setenv("OVMX_DECNET_EXECUTOR", pe, 1);
    setenv("OVMX_DECNET_NODEDB", pn, 1);
    setenv("OVMX_DECNET_OBJECTDB", po, 1);
    CHECK(strcmp(dnet_store_location(DNET_STORE_EXECUTOR), pe) == 0,
          "OVMX_DECNET_EXECUTOR selects the host path (the test hook)");
    {
        struct dnet_executor x, y;
        memset(&x, 0, sizeof(x));
        x.have_addr = 1; x.addr = (1u << 10) | 42u; strcpy(x.name, "OVMX"); x.state_on = 1;
        CHECK(dnet_store_save_executor(&x) == DNET_STORE_OK, "SAVE EXECUTOR via the hook");
        CHECK(dnet_store_load_executor(&y) == DNET_STORE_OK && y.have_addr &&
              y.addr == x.addr && strcmp(y.name, "OVMX") == 0 && y.state_on == 1,
              "executor 1.42 OVMX STATE ON round-trips");
        CHECK(file_has(pe, "OVMX layout, not the VMS NETNODE_LOCAL.DAT binary format"),
              "the executor file labels its layout as OVMX's (Rule 8)");
        CHECK(file_has(pe, "EXECUTOR 1.42 NAME OVMX STATE on"),
              "the executor record is the documented text layout");

        FILE *f = fopen(pe, "w");
        fputs("EXECUTOR 1.42 NAME OVMX\n", f);   /* truncated record */
        fclose(f);
        memset(&y, 0, sizeof(y));
        CHECK(dnet_store_load_executor(&y) == DNET_STORE_ECORRUPT && !y.have_addr,
              "a malformed executor record is CORRUPT and yields NO address (INV-6)");
    }
    {
        struct dnet_nodedb a, b;
        dnet_nodedb_init(&a);
        (void)dnet_nodedb_set(&a, (1u << 10) | 1u, "VAX1");
        (void)dnet_nodedb_set(&a, (1u << 10) | 2u, "");
        CHECK(dnet_store_save_nodes(&a) == DNET_STORE_OK, "SAVE NODES via the hook");
        CHECK(dnet_store_load_nodes(&b) == DNET_STORE_OK && b.count == 2 &&
              dnet_nodedb_by_name(&b, "vax1") != NULL,
              "the node database round-trips (name resolution intact)");
        CHECK(file_has(pn, "OVMX layout, not VMS NETNODE_REMOTE.DAT"),
              "the node file labels its layout as OVMX's");
    }
    {
        struct dnet_objectdb a, b;
        dnet_objectdb_init(&a);
        (void)dnet_objectdb_set(&a, 42, "CTERM", "");
        (void)dnet_objectdb_set(&a, 17, "FAL", "SYS$SYSTEM:DECNETD.EXE");
        CHECK(dnet_store_save_objects(&a) == DNET_STORE_OK, "SAVE OBJECTS via the hook");
        const struct dnet_object_entry *e = NULL;
        CHECK(dnet_store_load_objects(&b) == DNET_STORE_OK && b.count == 2 &&
              (e = dnet_objectdb_by_number(&b, 17)) != NULL &&
              strcmp(e->file, "SYS$SYSTEM:DECNETD.EXE") == 0,
              "the object database round-trips (number/name/file intact)");
    }
    unlink(pe); unlink(pn); unlink(po); rmdir(dir);

    printf("=== test_dnet_ncpstore: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
