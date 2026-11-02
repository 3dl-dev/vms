/*
 * test_acp_cache.c - the Files-11 ACP block cache (src/kernel-core/vms_acp_cache.h,
 * rd vms-943): the same header vms.ko runs, on the host. The cache may only ever
 * hand back the bytes on the disk: a block written underneath is read anew, a
 * read that raced a write is not kept, a unit's blocks go at $MOUNT/$DISMOUNT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vms_acp_cache.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); fails++; } } while (0)

#define NSET 4u
#define NBLK 4096u

static unsigned char disk[2][NBLK][VMS_ACP_CACHE_BLOCK];   /* two units */
static struct vms_acp_cache c;

/* the ACP's read through the cache, as vmsfs_acp.c does it */
static void rd(unsigned int unit, unsigned long long lbn, unsigned char *out)
{
	unsigned long long g;

	if (vms_acp_cache_lookup(&c, 8, unit, lbn, out))
		return;
	g = vms_acp_cache_gen(&c);
	memcpy(out, disk[unit][lbn], VMS_ACP_CACHE_BLOCK);
	vms_acp_cache_fill(&c, 8, unit, lbn, out, g);
}

static void wr(unsigned int unit, unsigned long long lbn, unsigned char v)
{
	memset(disk[unit][lbn], v, VMS_ACP_CACHE_BLOCK);
	vms_acp_cache_wrote(&c, 8, unit, lbn);
}

int main(void)
{
	struct vms_acp_cache_set **sets = calloc(NSET, sizeof(*sets));
	unsigned char b[VMS_ACP_CACHE_BLOCK];
	unsigned long long g, h0;
	unsigned int i, u, bad;

	for (i = 0; i < NSET; i++)
		sets[i] = calloc(1, sizeof(**sets));
	vms_acp_cache_init(&c, sets, NSET);
	for (u = 0; u < 2; u++)
		for (i = 0; i < NBLK; i++)
			memset(disk[u][i], (int)(u * 100 + i % 97 + 1), VMS_ACP_CACHE_BLOCK);

	/* 1. a block read twice is read from the disk once */
	rd(0, 7, b);
	h0 = c.hits;
	rd(0, 7, b);
	CHECK(c.hits == h0 + 1 && b[0] == disk[0][7][0], "second read of a block is a hit with the disk's bytes");

	/* 2. a header written underneath: the next read sees the new bytes */
	wr(0, 7, 0xAB);
	rd(0, 7, b);
	CHECK(b[0] == 0xAB && b[511] == 0xAB, "a block written after it was cached reads back the new bytes");
	rd(0, 7, b);
	CHECK(b[0] == 0xAB, "and keeps reading them");

	/* 3. a read that raced a write is used once and not kept */
	vms_acp_cache_wrote(&c, 8, 0, 9);          /* make sure 9 is not cached */
	g = vms_acp_cache_gen(&c);
	memcpy(b, disk[0][9], VMS_ACP_CACHE_BLOCK);  /* the read sees the OLD bytes ... */
	wr(0, 9, 0xCD);                              /* ... a write lands meanwhile ... */
	vms_acp_cache_fill(&c, 8, 0, 9, b, g);       /* ... the stale fill is refused */
	rd(0, 9, b);
	CHECK(b[0] == 0xCD, "a fill whose read began before a write is not kept");

	/* 4. same LBN on two units are two blocks */
	rd(0, 20, b); rd(1, 20, b);
	CHECK(b[0] == disk[1][20][0], "unit 1's block is not unit 0's");
	rd(0, 20, b);
	CHECK(b[0] == disk[0][20][0], "unit 0's block is not unit 1's");

	/* 5. $DISMOUNT/$MOUNT of a unit drops its blocks: new media reads anew */
	rd(1, 30, b);
	memset(disk[1][30], 0x5A, VMS_ACP_CACHE_BLOCK);   /* the medium changed */
	vms_acp_cache_drop_unit(&c, 8, 1);
	rd(1, 30, b);
	CHECK(b[0] == 0x5A, "after the unit's blocks are dropped, a read sees the new medium");
	rd(0, 20, b);
	CHECK(b[0] == disk[0][20][0], "another unit's blocks still read right");

	/* 6. far more blocks than the cache holds, written at random: every read
	 * equals the disk */
	srand(943);
	bad = 0;
	for (i = 0; i < 200000; i++) {
		unsigned int un = (unsigned)rand() & 1u;
		unsigned long long l = (unsigned)rand() % NBLK;

		if ((rand() % 5) == 0)
			wr(un, l, (unsigned char)rand());
		else {
			rd(un, l, b);
			if (memcmp(b, disk[un][l], VMS_ACP_CACHE_BLOCK) != 0)
				bad++;
		}
	}
	CHECK(bad == 0, "every read under eviction and writes equals the disk");
	CHECK(c.hits > 0 && c.fills > 0, "the cache was used");

	/* 7. drop_all (the node goes toward a cluster) empties it */
	vms_acp_cache_drop_all(&c);
	h0 = c.hits;
	rd(0, 7, b);
	CHECK(c.hits == h0, "after drop_all nothing is a hit");

	printf("acp_cache: hits=%llu misses=%llu fills=%llu stale=%llu inval=%llu\n",
	       c.hits, c.misses, c.fills, c.stale, c.inval);
	if (fails) {
		printf("test_acp_cache: %d FAILED\n", fails);
		return 1;
	}
	printf("test_acp_cache: PASS\n");
	return 0;
}
