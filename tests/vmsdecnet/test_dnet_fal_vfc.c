/*
 * test_dnet_fal_vfc.c -- rd vms-36aa: the FAL's record path keeps a VFC file
 * the VAX's size.
 *
 * A DAP DATA record of a VFC file is the fixed control area followed by the
 * data (the S1 wire: `01 8d 72 65 63 6f 72 64` = print control 01 8D,
 * "record"). The FAL stores it through RMS with the control in RAB$L_RHB and
 * reads it back the same way (dnet_fal_wput / dnet_fal_rget,
 * dnet_fal_search.c), so a file a VAX COPYs into OVMX's FAL holds the VAX's
 * own on-disk records: count word (2 + data), the control, the data, a pad
 * byte. Before RHB the FAL wrote a zeroed control area AND the control in
 * the data: BIGV.TXT, the VAX's 290-block BIG.TXT, was 297 blocks on OVMX
 * (vms-0a7 S1 run 4, case 7b).
 *
 * Real RMS (host mode, files under /tmp); no FAL hooks are stubbed.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dnet_fal.h"
#include "dnet_dap.h"

static int g_fail, g_pass;
#define CHECK(c, what) do { if (c) { g_pass++; printf("  ok: %s\n", what); } \
                            else { g_fail++; printf("  FAIL: %s\n", what); } } while (0)

/* Referenced by rms_parse.c; host mode has no logical-name resolution. */
int vmsfs_resolve(const char *spec, const char *def, char *res, size_t cap)
{ (void)spec; (void)def; (void)res; (void)cap; return -1; }

#define NREC 2000
static size_t mkrec(int i, uint8_t *b)
{
    b[0] = 0x01; b[1] = 0x8d;
    return 2 + (size_t)sprintf((char *)b + 2,
        "record %04d of the S1 long copy, abcdefghijklmnopqrstuvwxyz0123456789", i);
}

int main(void)
{
    char spec[128], disk[140], meta[160];
    snprintf(spec, sizeof spec, "/tmp/test_dnet_fal_vfc_%d.TXT", (int)getpid());
    snprintf(disk, sizeof disk, "%s;1", spec);
    snprintf(meta, sizeof meta, "%s.rms_meta", disk);

    void *h = NULL;
    int ok = dnet_fal_wopen(spec, DNET_DAP_RFM_VFC, DNET_DAP_RAT_PRN, 2, &h, NULL, 0) == 0;
    uint8_t rec[DNET_DAP_MAX_REC];
    for (int i = 1; ok && i <= NREC; i++) {
        size_t n = mkrec(i, rec);
        ok = dnet_fal_wput(h, rec, n) == 0;
    }
    ok = ok && dnet_fal_wclose(h) == 0;
    CHECK(ok, "the FAL stores 2000 DAP VFC records (01 8D + 69-byte text) through RMS");

    /* 71-byte record (2 + 69): count 71, control, text, pad = 74 bytes. */
    struct stat sb;
    CHECK(stat(disk, &sb) == 0 && sb.st_size == NREC * 74,
          "the stored file is 148000 bytes, EOF in block 290 -- the VAX's BIG.TXT, not 297 blocks");
    unsigned char raw[74];
    int fd = open(disk, O_RDONLY);
    int r1 = fd >= 0 && read(fd, raw, sizeof raw) == (ssize_t)sizeof raw && raw[0] == 71 &&
             raw[1] == 0 && raw[2] == 0x01 && raw[3] == 0x8d &&
             !memcmp(raw + 4, "record 0001 of", 14) && raw[73] == 0;
    if (fd >= 0) close(fd);
    CHECK(r1, "record 1 on disk: count 71, the control 01 8D in the fixed-control area, the text, a pad");

    void *rf = NULL;
    uint8_t rfm = 0, rat = 0;
    int same = dnet_fal_ropen(disk, &rf, &rfm, &rat) == 0 && rfm == DNET_DAP_RFM_VFC;
    int nread = 0, g;
    size_t len = 0;
    uint8_t want[DNET_DAP_MAX_REC];
    while (same && (g = dnet_fal_rget(rf, rec, sizeof rec, &len)) == 1) {
        size_t wn = mkrec(++nread, want);
        same = len == wn && !memcmp(rec, want, wn);
    }
    if (rf) (void)dnet_fal_rclose(rf);
    CHECK(same && nread == NREC,
          "the FAL reads each record back as the DAP record it took: 01 8D + the text, 2000 records");

    unlink(disk); unlink(meta);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
