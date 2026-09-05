/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mk_cluster_authorize.c - image-build-time CLUSTER_AUTHORIZE.DAT writer
 * (vms-ci.8 follow-on, E53).
 *
 * On real VMS, CLUSTER_CONFIG.COM writes CLUSTER_AUTHORIZE.DAT once, at
 * cluster-configuration time, onto the persistent system disk. OVMX's
 * stand-in (src/libvms/include/cluster_authorize.h) is a tiny typed file
 * read at boot by ovmx_init's load_cluster_sysgen_params(); there is no
 * interactive CLUSTER_CONFIG.COM equivalent yet (a disclosed limitation,
 * not a silent one -- see docs/cluster-integration-notes.md E53), so this
 * tool is the build-time stand-in for that one-time authoring step.
 *
 * It calls cluster_authorize_write() directly -- the SAME function/struct
 * layout the runtime reader (cluster_authorize_read()) parses -- so the
 * bytes this tool produces are byte-identical to what a real writer would
 * emit: no hand-packed struct, no guessed offsets, INV-6-honest.
 *
 * Usage: mk_cluster_authorize <output-path> <group#> [password]
 */
#include <stdio.h>
#include <stdlib.h>

#include "cluster_authorize.h"

int main(int argc, char **argv)
{
	unsigned long group_ul;
	uint16_t group;
	const char *password;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <output-path> <group#> [password]\n",
			argv[0]);
		return 2;
	}

	/* cluster_authorize_write()/_read() both resolve their path through
	 * OVMX_CLUSTER_AUTH_PATH (falling back to /etc/ovmx/cluster_authorize.dat)
	 * -- point that env var at the caller's requested output path rather
	 * than reimplementing the write. */
	if (setenv("OVMX_CLUSTER_AUTH_PATH", argv[1], 1) != 0) {
		fprintf(stderr, "mk_cluster_authorize: setenv failed\n");
		return 1;
	}

	group_ul = strtoul(argv[2], NULL, 10);
	if (group_ul > 0xffffu) {
		fprintf(stderr,
			"mk_cluster_authorize: group %lu does not fit CLUSTER_AUTHORIZE's uint16_t\n",
			group_ul);
		return 2;
	}
	group = (uint16_t)group_ul;
	password = (argc >= 4) ? argv[3] : "";

	if (cluster_authorize_write(group, password) != 0) {
		fprintf(stderr, "mk_cluster_authorize: write to %s failed\n", argv[1]);
		return 1;
	}

	fprintf(stderr, "mk_cluster_authorize: wrote group=%u to %s\n", group, argv[1]);
	return 0;
}
