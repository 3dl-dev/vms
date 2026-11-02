/* dev-only: put a host file onto a genuine ODS-2 image as a new version.
 * usage: ods2put <image> <DIR1/DIR2/...> <NAME.TYP> <hostfile> raw|stmlf */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "vmsfs/ods2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage\n"); return 2; }
    const char *comps[8]; int n = 0;
    char *d = strdup(argv[2]);
    for (char *t = strtok(d, "/"); t && n < 8; t = strtok(NULL, "/")) comps[n++] = t;
    FILE *fp = fopen(argv[4], "rb"); if (!fp) { perror(argv[4]); return 2; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); rewind(fp);
    uint8_t *buf = malloc(sz ? sz : 1);
    if (sz && fread(buf, 1, sz, fp) != (size_t)sz) return 2;
    fclose(fp);
    int fd = open(argv[1], O_RDWR); if (fd < 0) { perror(argv[1]); return 2; }
    ods2_bdev_t rd; ods2_fid_t dfid, cf, nf; uint8_t hdr[ODS2_BLOCK_SIZE]; uint16_t cv = 0, nv = 1;
    if (ods2_bdev_open(&rd, fd, 0) != ODS2_OK) { fprintf(stderr, "not ods2\n"); return 2; }
    if (ods2_bdev_resolve_dir(&rd, comps, n, &dfid, hdr, sizeof hdr) != ODS2_OK) { fprintf(stderr, "no dir\n"); return 2; }
    if (ods2_bdev_dir_find(&rd, hdr, argv[3], 0, &cf, &cv) == ODS2_OK) nv = cv + 1;
    ods2_wvolume_t w;
    if (ods2_wvolume_open_bdev(fd, 0, &w) != ODS2_OK) { fprintf(stderr, "wvol\n"); return 2; }
    ods2_status_t st = strcmp(argv[5], "raw") == 0
        ? ods2_wvolume_create_file_raw(&w, argv[3], nv, buf, sz, dfid, &nf)
        : ods2_wvolume_create_file_stmlf(&w, argv[3], nv, buf, sz, dfid, &nf);
    if (st != ODS2_OK) { fprintf(stderr, "create %d\n", (int)st); return 2; }
    if (ods2_wvolume_dir_insert(&w, dfid, argv[3], nv, nf) != ODS2_OK) { fprintf(stderr, "insert\n"); return 2; }
    ods2_wvolume_close(&w); fsync(fd); close(fd);
    printf("put %s;%u (%ld bytes)\n", argv[3], nv, sz);
    return 0;
}
