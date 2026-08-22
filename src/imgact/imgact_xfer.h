/*
 * imgact_xfer.h -- parse of an image's `.vms$xfer` section (vms-f60d).
 *
 * Factored out of imgact.c as a small, PURE (no I/O, no globals) translation
 * unit so it can be unit-tested on the host as well as compiled into the
 * freestanding IMGACT.EXE. It decides, from a mapped `.vms$xfer` section, HOW
 * IMGACT transfers to the image's entry (SysV tail-jump vs. VMS standard call)
 * and where the main transfer address is.
 *
 * The `.vms$xfer` section format (magic/flavor/count/entry_off[]) is defined in
 * ovmx_image.h (the LINK.EXE <-> IMGACT carrier contract).
 */
#ifndef OVMX_IMGACT_XFER_H
#define OVMX_IMGACT_XFER_H

#include <stdint.h>

/* Result of parsing a `.vms$xfer` section. `flavor` is OVMX_ACT_SYSV whenever
 * the section is absent, short, or malformed -- so a caller that treats SYSV as
 * "today's tail-jump path" gets ZERO regression for every non-`.vms$xfer`
 * image. `valid` is 1 only for a well-formed section. */
struct ovmx_xfer_info {
	unsigned int  flavor;    /* enum ovmx_act_flavor (0 == OVMX_ACT_SYSV)  */
	unsigned int  count;     /* number of transfer entries (0 when !valid) */
	uint64_t      main_off;  /* image-relative main transfer address       */
	int           valid;     /* 1 == a well-formed .vms$xfer was parsed     */
};

/*
 * Parse a mapped `.vms$xfer` section of `size` bytes at `sec`.
 *
 * Returns 1 and fills *out for a well-formed section (correct magic, count>=1,
 * enough bytes for the entry array); out->main_off is the LAST entry (the main
 * transfer address / __main), image-relative.
 *
 * Returns 0 for an absent (sec==0 or size==0), short, or bad-magic section and
 * sets out->flavor = OVMX_ACT_SYSV (0), out->valid = 0 -- the caller then uses
 * the unchanged tail-jump path. Never dereferences past `size`.
 */
int ovmx_parse_xfer(const void *sec, unsigned long size,
		    struct ovmx_xfer_info *out);

#endif /* OVMX_IMGACT_XFER_H */
