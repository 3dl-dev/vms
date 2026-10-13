/*
 * test_syssvc_lnm_cluster.c - LNM$SYSCLUSTER / LNM$CLUSTER_TABLE on OVMX, against
 * a real /dev/vms (rd vms-94a).
 *
 * The executive holds the clusterwide tables (vms_lnm.c,
 * VMS_LNM_TBL_CLUSTER[_DIR]) from its first moment. A member that propagates a
 * change lands it there (proven by tests/cluster/host and the vms-94a lab
 * fires). This suite proves the LOCAL half on a node that is in no cluster (the
 * QEMU harness never starts one):
 *   - $CRELNM in LNM$SYSCLUSTER (the logical name) and in LNM$SYSCLUSTER_TABLE
 *     makes the name, and $TRNLNM through either reads it back,
 *     multi-valued and mode-filtered, from the SAME arena entry;
 *   - the vmslnm manager API (DCL's DEFINE/TABLE, SHOW LOGICAL) reads it too;
 *   - $DELLNM removes it, and a second $DELLNM is SS$_NOLOGNAM;
 *   - LNM$CLUSTER_TABLE resolves as a table, and a name made there is not
 *     visible in LNM$SYSCLUSTER_TABLE.
 * The refusal of a local change while OTHER members share the cluster (no
 * origination yet) is proven on the host (test_lnm_cluster), where the
 * member count can be set; this harness has no cluster to share.
 *
 * NEGATIVE CONTROL: lnm-cluster-table-unresolved (tests/qemu/defects.d)
 * makes vms_kif_lnm_cluster_table() find nothing, reddening every
 * "resolves" assertion below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/logical.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static uint32_t cre(const char *table, const char *name, const char *v1,
                    const char *v2, uint8_t mode)
{
    struct dsc$descriptor_s td = mkdsc(table), nd = mkdsc(name);
    struct item_list_3 il[3];
    unsigned char m = mode;

    memset(il, 0, sizeof(il));
    il[0].buflen = (uint16_t)strlen(v1);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr = (void *)v1;
    if (v2) {
        il[1].buflen = (uint16_t)strlen(v2);
        il[1].item_code = LNM$_STRING;
        il[1].bufaddr = (void *)v2;
    }
    return sys$crelnm(NULL, &td, &nd, &m, il);
}

static uint32_t trn(const char *table, const char *name, int index, char *out,
                    size_t outsz)
{
    struct dsc$descriptor_s td = mkdsc(table), nd = mkdsc(name);
    struct item_list_3 il[3];
    uint16_t rl = 0;
    int32_t idx = index;

    memset(il, 0, sizeof(il));
    il[0].buflen = 4;
    il[0].item_code = LNM$_INDEX;
    il[0].bufaddr = &idx;
    il[1].buflen = (uint16_t)(outsz - 1);
    il[1].item_code = LNM$_STRING;
    il[1].bufaddr = out;
    il[1].retlen = &rl;
    out[0] = '\0';
    uint32_t st = sys$trnlnm(NULL, &td, &nd, NULL, il);
    if (rl >= outsz) rl = (uint16_t)(outsz - 1);
    out[rl] = '\0';
    return st;
}

static uint32_t del(const char *table, const char *name, uint8_t mode)
{
    struct dsc$descriptor_s td = mkdsc(table), nd = mkdsc(name);
    unsigned char m = mode;

    return sys$dellnm(&td, &nd, &m);
}

int main(void)
{
    char v[256];
    uint32_t w1 = 0, w2 = 0, w3 = 0, st;
    uint16_t rl = 0;
    uint32_t attrs = 0;
    lnm_manager_t *mgr;

    printf("test_syssvc_lnm_cluster: LNM$SYSCLUSTER on OVMX (rd vms-94a)\n");
    if (vms_kif_open() < 0) {
        printf("  SKIP: no /dev/vms\n");
        return EXIT_SKIP;
    }

    /* negctl: lnm-sysclustertable-absent */
    CHECK(vms_kif_lnm_cluster_table("LNM$SYSCLUSTER_TABLE", &w1) && w1 != 0u,
          "LNM$SYSCLUSTER_TABLE resolves to an executive clusterwide table");
    CHECK(vms_kif_lnm_cluster_table("LNM$CLUSTER_TABLE", &w2) && w2 != 0u &&
              w2 != w1,
          "LNM$CLUSTER_TABLE resolves to a different one");
    CHECK(!vms_kif_lnm_cluster_table("OVMX94A$NO_SUCH_TABLE", &w3),
          "a name that is no clusterwide table does not resolve");

    (void)del("LNM$SYSCLUSTER_TABLE", "OVMX94A$NAME", PSL$C_SUPER);
    st = cre("LNM$SYSCLUSTER", "OVMX94A$NAME", "ONE", "TWO", PSL$C_SUPER);
    CHECK((st & 1) != 0, "$CRELNM in LNM$SYSCLUSTER succeeds on a node in no cluster");

    st = trn("LNM$SYSCLUSTER_TABLE", "OVMX94A$NAME", 0, v, sizeof v);
    CHECK((st & 1) && strcmp(v, "ONE") == 0,
          "$TRNLNM in LNM$SYSCLUSTER_TABLE reads index 0 back");
    st = trn("LNM$SYSCLUSTER", "OVMX94A$NAME", 1, v, sizeof v);
    CHECK((st & 1) && strcmp(v, "TWO") == 0,
          "$TRNLNM through LNM$SYSCLUSTER reads index 1 of the same name");

    mgr = lnm_get_manager();
    st = lnm_translate(mgr, "LNM$SYSCLUSTER", "OVMX94A$NAME", v, sizeof v, &rl,
                       &attrs);
    CHECK((st & 1) && strcmp(v, "ONE") == 0,
          "the vmslnm manager (DCL's path) reads the same executive name");

    st = trn("LNM$CLUSTER_TABLE", "OVMX94A$NAME", 0, v, sizeof v);
    CHECK(!(st & 1), "it is not in LNM$CLUSTER_TABLE");

    /* Real V7.3 (lab94ag): "LNM$SYSTEM" = "LNM$SYSTEM_TABLE", "LNM$SYSCLUSTER". */
    st = trn("LNM$SYSTEM", "OVMX94A$NAME", 0, v, sizeof v);
    CHECK((st & 1) && strcmp(v, "ONE") == 0,
          "LNM$SYSTEM finds a LNM$SYSCLUSTER name (its second table)");
    st = trn("LNM$FILE_DEV", "OVMX94A$NAME", 0, v, sizeof v);
    CHECK((st & 1) && strcmp(v, "ONE") == 0,
          "LNM$FILE_DEV finds it too, after the process/job/group/system tables");
    st = trn("LNM$SYSTEM_TABLE", "OVMX94A$NAME", 0, v, sizeof v);
    CHECK(!(st & 1), "LNM$SYSTEM_TABLE alone does not");
    (void)del("LNM$SYSTEM_TABLE", "OVMX94A$NAME", PSL$C_SUPER);
    st = cre("LNM$SYSTEM_TABLE", "OVMX94A$NAME", "SYSWINS", NULL, PSL$C_SUPER);
    st = trn("LNM$SYSTEM", "OVMX94A$NAME", 0, v, sizeof v);
    CHECK((st & 1) && strcmp(v, "SYSWINS") == 0,
          "a LNM$SYSTEM_TABLE name of the same name is found first");
    (void)del("LNM$SYSTEM_TABLE", "OVMX94A$NAME", PSL$C_SUPER);

    st = del("LNM$SYSCLUSTER", "OVMX94A$NAME", PSL$C_SUPER);
    CHECK((st & 1) != 0, "$DELLNM removes it");
    st = trn("LNM$SYSCLUSTER_TABLE", "OVMX94A$NAME", 0, v, sizeof v);
    CHECK(!(st & 1), "  gone");
    st = del("LNM$SYSCLUSTER", "OVMX94A$NAME", PSL$C_SUPER);
    CHECK(st == SS$_NOLOGNAM, "a second $DELLNM is SS$_NOLOGNAM");

    printf("test_syssvc_lnm_cluster: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
