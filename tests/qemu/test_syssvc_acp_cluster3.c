/*
 * test_syssvc_acp_cluster3.c - the executive Files-11 ACP extends a file on a
 * volume with cluster factor 3 into the blocks BITMAP.SYS marks, never at the
 * cluster NUMBER taken as an LBN (rd vms-563), proven against a real /dev/vms.
 *
 * THE DEFECT. In the vms-5fa fire (tests/lab/captures/vms-5fa-shared-volume-
 * r2-20261102) OVMXE extended EVAC.DAT on VAX1's $2$DUA2, a volume INITIALIZEd
 * with cluster size 3: it cleared bitmap bit 29706 and wrote the file's data to
 * LBN 29706 -- the cluster number -- instead of LBNs 89118-89120, the cluster
 * that bit stands for. VAX1's ANALYZE/DISK_STRUCTURE: ALLOCCLR at LBN
 * 89118-89120, and LBN 29706 belonged to something else.
 *
 * THE VOLUME. SDA0: (the virtio-scsi disk; DKA700: on the Alpha rail) carries
 * /ods2_cluster3.img, mastered by tests/qemu/mkimage_ods2_cluster3.c: 3000
 * blocks, cluster factor 3, [000000]FILLER.DAT;1 (2000 blocks of 'F') then
 * [000000]EVAC.DAT;1 (2 blocks) ending at LBN 2048.
 *
 * WHAT IS PROVEN, through $MOUNT / IO$_ACCESS / IO$_WRITEVBLK / IO$_READVBLK
 * and an O_DIRECT view of the raw device that does not go through the ACP:
 *   1. Writing one block past EVAC.DAT's end extends it by one whole cluster
 *      (3 blocks; HIBLK 2 -> 5) and the block reads back, also after re-ACCESS.
 *   2. The new extent starts on a cluster boundary, and the raw block at the
 *      LBN the on-disk map gives holds the data.
 *   3. BITMAP.SYS and the map agree: exactly one bit went free -> allocated,
 *      and it is the cluster holding that LBN.
 *   4. FILLER.DAT still reads all 'F' -- the extend wrote nothing outside the
 *      cluster it allocated.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

#define MFD_NUM      4u
#define MFD_SEQ      4u
#define FILLER_BLKS  2000u
#define CLUSTER      3u

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

/* ---- raw O_DIRECT reads: no page cache can stand in for the platter ---- */
static int g_rawfd = -1;
static uint8_t *g_buf;

static int raw_read(uint32_t lbn, uint8_t *out)
{
    if (pread(g_rawfd, g_buf, 4096, (off_t)(lbn & ~7u) * 512) <
        (ssize_t)(((lbn & 7u) + 1u) * 512))
        return -1;
    memcpy(out, g_buf + (lbn & 7u) * 512, 512);
    return 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

/* The storage bitmap's first data block (BITMAP.SYS VBN2): FID 2's header is
 * at hm2_ibmaplbn + hm2_ibmapsize + 1; its first retrieval pointer (format 1)
 * starts at the SCB (VBN1). The volume's 1000 clusters fit in that one block. */
static uint32_t raw_bitmap_lbn(void)
{
    uint8_t home[512], hdr[512];
    uint32_t hdr_lbn, w0;
    unsigned mp;

    if (raw_read(1, home) != 0)
        return 0;
    hdr_lbn = rd32(home + 24) + rd16(home + 32) + 1u;
    if (raw_read(hdr_lbn, hdr) != 0)
        return 0;
    mp = (unsigned)hdr[1] * 2u;
    w0 = rd16(hdr + mp);
    if ((w0 >> 14) != 1u)
        return 0;
    return ((((w0 >> 8) & 0x3Fu) << 16) | rd16(hdr + mp + 2)) + 1u;
}

static int bm_free(const uint8_t *bm, uint32_t bit)
{
    return (bm[bit / 8u] >> (bit % 8u)) & 1u;
}

static uint32_t access_file(uint32_t chan, const char *name, int write,
                            uint32_t probe_vbn, struct vms_acp_access_args *a)
{
    memset(a, 0, sizeof(*a));
    a->chan = chan;
    a->did_num = MFD_NUM;
    a->did_seq = MFD_SEQ;
    a->probe_vbn = probe_vbn;
    if (write)
        a->acctl = VMS_ACP_ACCTL_WRITE;
    strncpy(a->name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_access(a);
}

static void rw_init(struct vms_acp_rw_args *r, uint32_t chan, uint32_t vbn,
                    uint32_t length, void *buf)
{
    memset(r, 0, sizeof(*r));
    r->chan = chan;
    r->vbn = vbn;
    r->length = length;
    r->buffer = (uint64_t)(uintptr_t)buf;
}

int main(void)
{
    static const char *const units[] = { "SDA0:", "DKA700:" };
    struct vms_acp_access_args a;
    struct vms_acp_rw_args r;
    uint8_t pat[512], blk[512], bm_before[512], bm_after[512], *fbuf;
    char backing[64], path[80];
    const char *unit = NULL;
    uint32_t maj = 0, min = 0, st, chan = 0, old_hiblk = 0, lbn = 0, bm_lbn = 0;
    uint32_t k, changed = 0, changed_bit = 0, v;
    int filler_ok = 1;
    unsigned i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_acp_cluster3: the ACP extends a file on a cluster-factor-3 "
           "volume into the cluster BITMAP.SYS marks (rd vms-563) ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    {
        int fd = vms_kif_open();
        if (fd < 0) {
            printf("=== test_syssvc_acp_cluster3: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
            return EXIT_SKIP;
        }
        vms_kif_close();
    }

    for (i = 0; i < sizeof(units) / sizeof(units[0]); i++)
        if (vms_kif_disk_resolve(units[i], backing, sizeof(backing), &maj, &min) == SS$_NORMAL) {
            unit = units[i];
            break;
        }
    check(unit != NULL, "the cluster-factor-3 fixture disk is attached (SDA0: / DKA700:)");
    if (!unit) {
        printf("=== test_syssvc_acp_cluster3: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    snprintf(path, sizeof(path), "/dev/%s", backing);
    g_rawfd = open(path, O_RDONLY | O_DIRECT);
    if (posix_memalign((void **)&g_buf, 4096, 4096) != 0)
        g_buf = NULL;
    check(g_rawfd >= 0 && g_buf != NULL, "raw O_DIRECT view of the backing device (independent of the ACP)");
    check(g_rawfd >= 0 && g_buf && raw_read(1, blk) == 0 && rd16(blk + 14) == CLUSTER,
          "the volume's home block says cluster factor 3 (precondition)");
    bm_lbn = (g_rawfd >= 0 && g_buf) ? raw_bitmap_lbn() : 0;
    check(bm_lbn != 0 && raw_read(bm_lbn, bm_before) == 0,
          "BITMAP.SYS's first bitmap block is located and read (precondition)");

    st = vms_kif_acp_mount(unit);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT the cluster-factor-3 volume (precondition)");
    st = vms_kif_acp_assign(unit, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel (precondition)");
    if (chan == 0) {
        printf("=== test_syssvc_acp_cluster3: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* ---- 1. extend EVAC.DAT by writing one block past its end ---- */
    st = access_file(chan, "EVAC.DAT", 1, 0, &a);
    old_hiblk = a.attr.hiblk;
    check($VMS_STATUS_SUCCESS(st) && old_hiblk == 2, "IO$_ACCESS EVAC.DAT for write (2 blocks allocated)");
    for (i = 0; i < 512; i++)
        pat[i] = (uint8_t)(0x63u ^ (i * 7u));
    rw_init(&r, chan, old_hiblk + 1u, 512, pat);
    st = vms_kif_acp_writevb(&r);
    check($VMS_STATUS_SUCCESS(st) && r.extended == CLUSTER && r.new_hiblk == old_hiblk + CLUSTER,
          "IO$_WRITEVBLK one block past the end extends EVAC.DAT by one whole cluster (3 blocks)");
    (void)vms_kif_acp_deaccess(chan);

    st = access_file(chan, "EVAC.DAT", 0, old_hiblk + 1u, &a);
    lbn = a.probe_lbn;
    check($VMS_STATUS_SUCCESS(st) && a.attr.hiblk == old_hiblk + CLUSTER && lbn != 0,
          "after re-ACCESS the on-disk header holds the grown allocation (HIBLK 5)");
    memset(blk, 0, sizeof(blk));
    rw_init(&r, chan, old_hiblk + 1u, 512, blk);
    st = vms_kif_acp_readvb(&r);
    check($VMS_STATUS_SUCCESS(st) && memcmp(blk, pat, 512) == 0,
          "after re-ACCESS the extended VBN reads back its data");
    (void)vms_kif_acp_deaccess(chan);

    /* ---- 2. where it went, read off the platter ---- */
    printf("  INFO: extended VBN %u -> LBN %u (cluster %u)\n",
           (unsigned)(old_hiblk + 1u), (unsigned)lbn, (unsigned)(lbn / CLUSTER));
    /* negctl: acp-cluster-lbn-not-scaled */
    check(lbn != 0 && lbn % CLUSTER == 0,
          "the new extent starts on a cluster boundary (LBN a multiple of 3)");
    check(lbn != 0 && raw_read(lbn, blk) == 0 && memcmp(blk, pat, 512) == 0,
          "the raw block at the LBN the on-disk map gives holds the data");

    /* ---- 3. BITMAP.SYS and the map agree ---- */
    check(raw_read(bm_lbn, bm_after) == 0, "BITMAP.SYS re-read after the extend");
    for (k = 0; k < 3000u / CLUSTER; k++)
        if (bm_free(bm_before, k) != bm_free(bm_after, k)) {
            changed++;
            changed_bit = k;
        }
    printf("  INFO: %u bitmap bit(s) changed; last %u\n", (unsigned)changed, (unsigned)changed_bit);
    check(changed == 1 && bm_free(bm_before, changed_bit) && changed_bit == lbn / CLUSTER,
          "exactly one BITMAP.SYS bit went free -> allocated, the cluster holding the new data");

    /* ---- 4. no other file was written ---- */
    fbuf = malloc(64u * 512u);
    st = access_file(chan, "FILLER.DAT", 0, 0, &a);
    check($VMS_STATUS_SUCCESS(st) && a.attr.efblk >= FILLER_BLKS && fbuf != NULL,
          "IO$_ACCESS FILLER.DAT (2000 blocks)");
    for (v = 1; fbuf && $VMS_STATUS_SUCCESS(st) && v <= FILLER_BLKS; v += 64u) {
        uint32_t n = FILLER_BLKS - v + 1u < 64u ? FILLER_BLKS - v + 1u : 64u;
        rw_init(&r, chan, v, n * 512u, fbuf);
        if (!$VMS_STATUS_SUCCESS(vms_kif_acp_readvb(&r)) || r.xferred != n * 512u) {
            filler_ok = 0;
            break;
        }
        for (k = 0; k < n * 512u; k++)
            if (fbuf[k] != 'F') {
                filler_ok = 0;
                printf("  INFO: FILLER.DAT VBN %u differs\n", (unsigned)(v + k / 512u));
                break;
            }
        if (!filler_ok)
            break;
    }
    check($VMS_STATUS_SUCCESS(st) && filler_ok,
          "FILLER.DAT still reads all 'F': the extend wrote nothing outside its cluster");
    (void)vms_kif_acp_deaccess(chan);
    free(fbuf);

    (void)vms_kif_dassgn(chan);
    st = vms_kif_acp_dmount(unit);
    check($VMS_STATUS_SUCCESS(st), "$DISMOUNT the volume");

    printf("=== test_syssvc_acp_cluster3: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
