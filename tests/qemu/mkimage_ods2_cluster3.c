/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mkimage_ods2_cluster3.c - master the cluster-factor-3 ODS-2 volume the
 * test_syssvc_acp_cluster3 suite mounts (rd vms-563).
 *
 * A 3000-block volume labelled CLUSTER3, formatted by the ODS-2 writer, with
 * [000000]FILLER.DAT;1 (2000 blocks) and then [000000]EVAC.DAT;1 (600 bytes,
 * 2 blocks) after it, then converted to cluster factor 3
 * by tests/ods2/ods2_cluster_fixture.h (home blocks, SCB, storage bitmap; see
 * that header for exactly what changes and why it is a fixture, not an
 * INITIALIZE). VAX1's $2$DUA2 in the vms-5fa fire was INITIALIZEd with
 * cluster size 3; the ACP's extend there wrote data at the cluster number as
 * an LBN. The filler puts EVAC.DAT's end -- where an extend searches first --
 * near LBN 2000, so the cluster NUMBER there (~680) taken as an LBN lands in
 * FILLER.DAT's data, not in the volume's metadata: a regression shows up as a
 * wrong-place extent the suite names, never as a torn index file. Staged as /ods2_cluster3.img; run_tests.sh puts it on the virtio-scsi
 * disk (SDA0:).
 *
 * Usage: mkimage_ods2_cluster3 <out.img>
 */

#include "vmsfs/ods2.h"
#include "ods2_cluster_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOTAL_BLOCKS 3000u

int main(int argc, char **argv)
{
    size_t len = (size_t)TOTAL_BLOCKS * ODS2_BLOCK_SIZE;
    uint8_t *image, data[600], *filler;
    ods2_format_params_t params;
    ods2_wvolume_t wvol;
    ods2_fid_t fid;
    ods2_status_t st;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <out.img>\n", argv[0]);
        return 2;
    }
    image = calloc(1, len);
    if (!image)
        return 1;
    params.total_blocks = TOTAL_BLOCKS;
    params.maxfiles = 32;
    params.volname = "CLUSTER3";
    st = ods2_volume_format(image, len, &params, &wvol);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_cluster3: format: %s\n", ods2_strerror(st));
        return 1;
    }
    filler = calloc(2000, ODS2_BLOCK_SIZE);
    if (!filler)
        return 1;
    memset(filler, 'F', (size_t)2000 * ODS2_BLOCK_SIZE);
    st = ods2_wvolume_create_file_raw(&wvol, "FILLER.DAT", 1, filler,
                                      (size_t)2000 * ODS2_BLOCK_SIZE, wvol.mfd_fid, &fid);
    if (st == ODS2_OK)
        st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "FILLER.DAT", 1, fid);
    free(filler);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_cluster3: FILLER.DAT: %s\n", ods2_strerror(st));
        return 1;
    }
    memset(data, 'E', sizeof(data));
    st = ods2_wvolume_create_file_raw(&wvol, "EVAC.DAT", 1, data, sizeof(data),
                                      wvol.mfd_fid, &fid);
    if (st == ODS2_OK)
        st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "EVAC.DAT", 1, fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_cluster3: EVAC.DAT: %s\n", ods2_strerror(st));
        return 1;
    }
    if (ods2_fixture_recluster(image, len, &wvol, 3) != 0) {
        fprintf(stderr, "mkimage_ods2_cluster3: cluster conversion failed\n");
        return 1;
    }
    f = fopen(argv[1], "wb");
    if (!f || fwrite(image, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "mkimage_ods2_cluster3: cannot write %s\n", argv[1]);
        return 1;
    }
    printf("mkimage_ods2_cluster3: %s: %u blocks, cluster 3, [000000]EVAC.DAT;1 FID (%u,%u,%u)\n",
           argv[1], TOTAL_BLOCKS, (unsigned)ods2_fid_number(&fid), fid.fid_seq, fid.fid_rvn);
    free(image);
    return 0;
}
