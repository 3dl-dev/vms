/* dbparse.c -- run the REAL codec (vms_cm_lnm_db_next + vms_cm_lnm_rec_parse)
 * over a pulled clusterwide logical-name database dumped to a file, and say
 * which record, if any, this node cannot decode. Diagnostic only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vms_cluster_codec_cm.h"

int main(int argc, char **argv)
{
	static unsigned char db[0x91e8];
	struct vms_cm_lnm_op op;
	unsigned int cursor = 0, n = 0, rec_len = 0;
	const unsigned char *rec;
	vms_codec_status_t cs;
	FILE *f;
	size_t len;

	if (argc < 2) { fprintf(stderr, "usage: dbparse <db.bin>\n"); return 2; }
	f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	len = fread(db, 1, sizeof(db), f);
	fclose(f);
	printf("%zu bytes; header len=%u fn=%u count=%u\n", len,
	       (unsigned)(db[0] | db[1] << 8), (unsigned)(db[2] | db[3] << 8),
	       vms_cm_lnm_db_count(db, (unsigned)len));
	for (;;) {
		cs = vms_cm_lnm_db_next(db, (unsigned)len, &cursor, &rec, &rec_len);
		if (cs == VMS_CODEC_E_RANGE) { printf("end after %u records\n", n); break; }
		if (cs != VMS_CODEC_OK) {
			printf("CONTAINER REFUSED at cursor %u: status %d\n", cursor, (int)cs);
			break;
		}
		cs = vms_cm_lnm_rec_parse(rec, rec_len, &op);
		if (cs != VMS_CODEC_OK) {
			printf("record %u at %u (%u bytes) REFUSED: status %d  fn=%u n=%u head=",
			       n, cursor - rec_len, rec_len, (int)cs,
			       (unsigned)(rec[2] | rec[3] << 8),
			       (unsigned)(rec[8] | rec[9] << 8 | rec[10] << 16 | rec[11] << 24));
			for (unsigned i = 0; i < (rec_len < 48 ? rec_len : 48); i++)
				printf("%02x", rec[i]);
			printf("\n");
		} else {
			printf("record %u at %u (%u bytes) fn=%u table=%s name=%s equiv=%u acmode=%u\n",
			       n, cursor - rec_len, rec_len, op.fn, op.table.s,
			       op.name.s, op.n_equiv, op.acmode);
		}
		n++;
	}
	return 0;
}
