/* SPDX-License-Identifier: GPL-2.0 */
/* sim_rng.c - see sim_rng.h for why every random decision in the rung-2
 * simulator comes from here and from a per-link stream. */

#include "sim_rng.h"

/* SplitMix64: a full-period mixer used ONLY to turn a (seed, stream) pair into
 * a well-distributed 64-bit state. Published, public domain. */
static uint64_t splitmix64(uint64_t *x)
{
	uint64_t z;

	*x += 0x9e3779b97f4a7c15ull;
	z = *x;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

void sim_rng_seed(struct sim_rng *r, uint64_t seed, uint64_t stream)
{
	uint64_t x = seed ^ (stream * 0xda942042e4dd58b5ull);

	r->draws = 0u;
	/* xorshift64* has no valid zero state, so mix until it is not zero.
	 * splitmix64 is a bijection, so this terminates on the first or (at
	 * worst) the second call and is still perfectly deterministic. */
	do {
		r->s = splitmix64(&x);
	} while (r->s == 0ull);
}

uint64_t sim_rng_u64(struct sim_rng *r)
{
	uint64_t x = r->s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	r->s = x;
	r->draws++;
	return x * 0x2545f4914f6cdd1dull;
}

uint32_t sim_rng_u32(struct sim_rng *r)
{
	return (uint32_t)(sim_rng_u64(r) >> 32);
}

/* Resolution of a link-condition probability: hundredths of a per cent. */
#define SIM_PCT_SCALE 10000u

int sim_rng_pct(struct sim_rng *r, uint32_t pct)
{
	uint32_t threshold, draw;

	if (pct == 0u)
		return 0;          /* costs no draw -- see the header */
	if (pct >= 100u)
		return 1;

	threshold = pct * 100u;
	/* Rejection sampling on the top of the 32-bit range, so the result is
	 * an exact threshold/SIM_PCT_SCALE and not a modulo-biased one. */
	do {
		draw = sim_rng_u32(r);
	} while (draw >= 0xffffffffu - (0xffffffffu % SIM_PCT_SCALE));
	return (draw % SIM_PCT_SCALE) < threshold;
}

uint32_t sim_rng_range(struct sim_rng *r, uint32_t lo, uint32_t hi)
{
	uint32_t span;

	if (hi <= lo)
		return lo;
	span = hi - lo + 1u;
	return lo + (sim_rng_u32(r) % span);
}
