/* SPDX-License-Identifier: GPL-2.0 */
/*
 * conformance_dump.h - the frame-conformance gate's emission hook (rd vms-ci.6).
 *
 * A host test that drives one of OVMX's REAL senders calls conf_dump() with
 * every frame (or SYSAP body) that sender emitted. When the environment names
 * a file in OVMX_CONFORMANCE_OUT the bytes are appended there, one line each:
 *
 *     <frame-type> <base-abs-offset> <state k=v,...|-> <hex>
 *
 * and tools/cluster/frame_conformance.py checks every byte against the
 * grounding table tests/cluster/conformance/frame-grounding.tsv. Without the
 * variable this is a no-op, so the tests behave exactly as before.
 */
#ifndef OVMX_CONFORMANCE_DUMP_H
#define OVMX_CONFORMANCE_DUMP_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static inline void conf_dump(const char *type, unsigned base,
			     const char *state, const uint8_t *b, uint32_t n)
{
	const char *path = getenv("OVMX_CONFORMANCE_OUT");
	FILE *fp;
	uint32_t i;

	if (path == NULL || path[0] == '\0' || b == NULL)
		return;
	fp = fopen(path, "a");
	if (fp == NULL)
		return;
	fprintf(fp, "%s %u %s ", type, base,
		(state != NULL && state[0] != '\0') ? state : "-");
	for (i = 0; i < n; i++)
		fprintf(fp, "%02x", b[i]);
	fputc('\n', fp);
	fclose(fp);
}

#endif /* OVMX_CONFORMANCE_DUMP_H */
