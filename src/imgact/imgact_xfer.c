/*
 * imgact_xfer.c -- pure parse of an image's `.vms$xfer` section (vms-f60d).
 *
 * See imgact_xfer.h. Freestanding: no libc, no globals, no I/O -- suitable both
 * for the -nostdlib IMGACT.EXE build and for a plain host unit test.
 */
#include "imgact_xfer.h"
#include "ovmx_image.h"   /* OVMX_XFER_MAGIC, enum ovmx_act_flavor, header */

int ovmx_parse_xfer(const void *sec, unsigned long size,
		    struct ovmx_xfer_info *out)
{
	out->flavor   = OVMX_ACT_SYSV;   /* default: today's tail-jump path */
	out->count    = 0;
	out->main_off = 0;
	out->valid    = 0;

	if (!sec || size < sizeof(struct ovmx_xfer_header))
		return 0;

	const struct ovmx_xfer_header *h = (const struct ovmx_xfer_header *)sec;
	if (h->magic != OVMX_XFER_MAGIC)
		return 0;
	if (h->count == 0)
		return 0;

	/* Enough bytes for the header + count quadword entries? Guard against
	 * count overflowing the byte math (count is a 32-bit field). */
	unsigned long need = sizeof(struct ovmx_xfer_header)
			   + (unsigned long)h->count * sizeof(uint64_t);
	if (need < sizeof(struct ovmx_xfer_header) || size < need)
		return 0;

	const uint64_t *entries =
		(const uint64_t *)((const char *)sec
				   + sizeof(struct ovmx_xfer_header));

	out->flavor   = h->flavor;
	out->count    = h->count;
	out->main_off = entries[h->count - 1];  /* last == main transfer addr */
	out->valid    = 1;
	return 1;
}
