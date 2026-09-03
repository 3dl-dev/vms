/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_rng.h - the simulator's determinism substrate: a seeded PRNG with
 * INDEPENDENT PER-LINK STREAMS (FC-P1.4).
 *
 * WHY THIS FILE EXISTS AT ALL. The plan's done-condition for the rung-2
 * harness is "deterministic by seed": the same seed must produce a
 * bit-identical run, or a red scenario is not reproducible and the harness is
 * worse than no harness. Everything random in the simulator -- which frame the
 * virtual LAN drops, which it duplicates, which it delays past its successor --
 * comes from here and from nowhere else. There is no call to rand(), no
 * time(NULL), no address-of-anything folded into a hash.
 *
 * WHY PER-LINK STREAMS, AND NOT ONE GLOBAL SEQUENCE. With a single stream, the
 * loss pattern on link A->B depends on how many draws links A->C and B->C
 * happened to make first. Add a fourth node to a scenario and every existing
 * link's behaviour reshuffles, so a scenario that was green yesterday goes red
 * today for a reason that has nothing to do with the code under test. Each
 * directed link therefore gets its own stream, seeded from (master seed, link
 * identity), and draws on it are independent of every other link's traffic.
 *
 * THE ALGORITHM IS PUBLIC AND IS NOT VMS. SplitMix64 (Steele/Lea/Flood, 2014)
 * to expand the seed, xorshift64* (Vigna, 2014) for the stream. Both are
 * published, public-domain generators; this file reverse-engineers nothing and
 * has nothing to do with the cluster protocol (Rule 8 is about VMS's
 * unpublished algorithms -- a test harness's dice are not one of them).
 *
 * HOST-ONLY. tests/cluster/sim/ is a rung-2 host harness, never compiled into
 * vms.ko or the NetBSD kmod; it may use the C library freely.
 */
#ifndef OVMX_SIM_RNG_H
#define OVMX_SIM_RNG_H

#include <stdint.h>

/* One stream. Copyable by value: a scenario that wants to peek ahead can take
 * a copy and draw from it without disturbing the run. */
struct sim_rng {
	uint64_t s;
	uint64_t draws;   /* how many numbers this stream has produced */
};

/*
 * Seed one stream from a master seed and a stream identity. `stream` is the
 * per-link discriminator (see the file comment); two different identities on
 * the same master seed give two independent sequences, and the same pair
 * always gives the same sequence.
 */
void sim_rng_seed(struct sim_rng *r, uint64_t seed, uint64_t stream);

/* The next 64 and 32 bits of the stream. */
uint64_t sim_rng_u64(struct sim_rng *r);
uint32_t sim_rng_u32(struct sim_rng *r);

/*
 * A Bernoulli draw at `pct` PER CENT, resolved to a hundredth of a per cent so
 * a 10 % link condition is exactly 1000/10000 and not a modulo-biased
 * approximation of it. pct == 0 never fires and consumes NO draw (a link with
 * a condition turned off must not perturb the stream of a link that has one);
 * pct >= 100 always fires, also without a draw.
 */
int sim_rng_pct(struct sim_rng *r, uint32_t pct);

/* A uniform integer in [lo, hi]. hi < lo returns lo and consumes no draw. */
uint32_t sim_rng_range(struct sim_rng *r, uint32_t lo, uint32_t hi);

#endif /* OVMX_SIM_RNG_H */
