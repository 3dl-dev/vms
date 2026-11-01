/* SPDX-License-Identifier: GPL-2.0 */
/*
 * show_cluster_authorize.c - print the cluster group a CLUSTER_AUTHORIZE
 * record carries (rd vms-daa).
 *
 * WHY THIS EXISTS. The cluster group number is what the LAVC HELLO multicast
 * address is built from (AB-00-04-01-<LE16(group + 0x100)>,
 * src/kernel-core/vms_cluster_codec_hello.c), so a node booted with group 0 is
 * on a different multicast group from the cluster it was meant to join: it
 * transmits into the void, receives nothing, and NEVER JOINS -- silently. An
 * ordinary OVMX image build ships an empty /etc/ovmx and therefore group 0
 * (tests/lab/tools/stage_cluster_group.sh is what puts a real group in one),
 * and a lab fire that boots such an image burns the whole run before anybody
 * notices. tests/lab/tools/check_cluster_group.sh is the precondition that
 * catches it, and this is how it READS the record.
 *
 * It parses NO bytes of its own: cluster_authorize_read() -- the same function
 * the runtime reader uses (src/libvms/include/cluster_authorize.h) -- is what
 * reads the record, so this tool cannot disagree with the executive about what
 * a record says. One parser, not two.
 *
 * Usage: show_cluster_authorize <record-path>
 * Prints: the group number, decimal, on stdout.
 * Exit:   0 the record was read; 1 it could not be (missing, bad magic or
 *         version -- which the executive would read as no record at all, i.e.
 *         group 0); 2 usage.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cluster_authorize.h"

int main(int argc, char **argv)
{
	struct cluster_authorize rec;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <record-path>\n", argv[0]);
		return 2;
	}

	/* Both halves of this header resolve their path through
	 * OVMX_CLUSTER_AUTH_PATH (mk_cluster_authorize.c does the same), so
	 * point it at the caller's file rather than reimplementing the read. */
	if (setenv("OVMX_CLUSTER_AUTH_PATH", argv[1], 1) != 0) {
		fprintf(stderr, "show_cluster_authorize: setenv failed\n");
		return 1;
	}
	if (cluster_authorize_read(&rec) != 0) {
		fprintf(stderr, "show_cluster_authorize: %s is not a readable "
				"CLUSTER_AUTHORIZE record\n", argv[1]);
		return 1;
	}
	printf("%u\n", (unsigned)rec.group);
	return 0;
}
