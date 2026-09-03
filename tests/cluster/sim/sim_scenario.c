/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_scenario.c - the scenario runner: build the cluster a scenario declares,
 * execute its steps in order, and report. See sim_scenario.h for the DSL a
 * later phase's author actually writes.
 *
 * One small handler per step kind, dispatched through a table indexed by the
 * kind -- the same shape the FSMs under test use, for the same reason: a step
 * kind is a row somebody can read, not a branch buried in a ladder.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sim_scenario.h"

/* Everything a step handler needs. */
struct sim_run_ctx {
	const struct sim_scenario *scn;
	struct sim                *s;
	struct sim_scenario_out   *out;
	uint32_t                   step_index;
	const struct sim_step     *step;
};

/* ------------------------------------------------------------------ *
 * Reporting
 * ------------------------------------------------------------------ */

static void record_failure(struct sim_run_ctx *c, const char *fmt, ...)
{
	struct sim_scenario_out *o = c->out;
	va_list ap;

	o->failures++;
	if (o->n_recorded >= SIM_MAX_FAILURES)
		return;
	o->failure[o->n_recorded].step = c->step_index;
	va_start(ap, fmt);
	vsnprintf(o->failure[o->n_recorded].text,
		  sizeof(o->failure[0].text), fmt, ap);
	va_end(ap);
	o->n_recorded++;
}

/* A node a step named but the scenario never declared: a scenario bug, and it
 * is reported as a failure rather than silently doing nothing. */
static struct sim_node *need_node(struct sim_run_ctx *c, const char *name)
{
	struct sim_node *n = sim_node_by_name(c->s, name);

	if (n == NULL)
		record_failure(c, "step %u names unknown node '%s'",
			       c->step_index, name != NULL ? name : "(null)");
	return n;
}

/* ------------------------------------------------------------------ *
 * Step handlers -- one per kind
 * ------------------------------------------------------------------ */

static void step_link(struct sim_run_ctx *c)
{
	struct sim_node *a = need_node(c, c->step->a);
	struct sim_node *b = need_node(c, c->step->b);

	if (a == NULL || b == NULL)
		return;
	sim_lan_set_link(&c->s->lan, a->index, b->index, &c->step->link);
}

static void step_link_pair(struct sim_run_ctx *c)
{
	struct sim_node *a = need_node(c, c->step->a);
	struct sim_node *b = need_node(c, c->step->b);

	if (a == NULL || b == NULL)
		return;
	sim_lan_set_link(&c->s->lan, a->index, b->index, &c->step->link);
	sim_lan_set_link(&c->s->lan, b->index, a->index, &c->step->link);
}

static void step_link_all(struct sim_run_ctx *c)
{
	sim_lan_set_link_all(&c->s->lan, &c->step->link);
}

static void step_cut(struct sim_run_ctx *c)
{
	struct sim_node *a = need_node(c, c->step->a);
	struct sim_node *b = need_node(c, c->step->b);

	if (a == NULL || b == NULL)
		return;
	/* A partition is symmetric: a scenario that wants a one-way failure
	 * says so with SIM_LINK(.loss_pct = 100). */
	sim_lan_cut(&c->s->lan, a->index, b->index, c->step->flag);
	sim_lan_cut(&c->s->lan, b->index, a->index, c->step->flag);
}

static void step_node_link(struct sim_run_ctx *c)
{
	struct sim_node *n = need_node(c, c->step->a);

	if (n != NULL)
		sim_node_set_link(n, c->step->flag);
}

static void step_halt(struct sim_run_ctx *c)
{
	struct sim_node *n = need_node(c, c->step->a);

	if (n != NULL)
		sim_node_halt(n);
}

static void step_boot(struct sim_run_ctx *c)
{
	struct sim_node *n = need_node(c, c->step->a);

	if (n == NULL)
		return;
	if (sim_node_boot(n) != 0)
		record_failure(c, "node '%s' refused to boot (identity the "
				  "port will not assert)", n->cfg.name);
}

static void step_run(struct sim_run_ctx *c)
{
	(void)sim_run_ms(c->s, c->step->ms);
}

static void step_until_vcs(struct sim_run_ctx *c)
{
	if (!sim_run_until(c->s, sim_all_vcs_open, NULL, c->step->ms))
		record_failure(c,
			       "not every circuit reached OPEN within %u ms "
			       "(%u of %u open)", c->step->ms,
			       sim_count_vcs_open(c->s),
			       c->s->n_nodes * (c->s->n_nodes - 1u));
}

static void step_until_channels(struct sim_run_ctx *c)
{
	if (!sim_run_until(c->s, sim_all_channels_verified, NULL, c->step->ms))
		record_failure(c,
			       "not every channel verified within %u ms "
			       "(%u of %u at b4)", c->step->ms,
			       sim_count_channels_verified(c->s),
			       c->s->n_nodes * (c->s->n_nodes - 1u));
}

static void step_send(struct sim_run_ctx *c)
{
	struct sim_node *a = need_node(c, c->step->a);
	struct sim_node *b = need_node(c, c->step->b);

	if (a == NULL || b == NULL)
		return;
	(void)sim_send_msgs(c->s, a, b, c->step->count);
	/* A refusal is a real answer from the port (no circuit, no credit,
	 * ring full) and is left in the node's counters for an expectation to
	 * assert. The step does not retry behind the scenario's back. */
}

/* ---- expectations ---- */

static int cmp_holds(enum sim_cmp op, uint64_t got, uint64_t want)
{
	switch (op) {
	case SIM_CMP_EQ: return got == want;
	case SIM_CMP_GE: return got >= want;
	case SIM_CMP_LE: return got <= want;
	case SIM_CMP_GT: return got >  want;
	case SIM_CMP_LT: return got <  want;
	case SIM_CMP_NE: return got != want;
	default:         return 0;
	}
}

static const char *cmp_name(enum sim_cmp op)
{
	static const char *const n[] = { "==", ">=", "<=", ">", "<", "!=" };

	return (unsigned)op < sizeof(n) / sizeof(n[0]) ? n[op] : "?";
}

static void step_expect(struct sim_run_ctx *c)
{
	const struct sim_step *st = c->step;
	enum sim_metric m = (enum sim_metric)st->metric;
	uint64_t got = sim_metric_read(c->s, m, st->a, st->b);

	c->out->checks++;
	if (cmp_holds((enum sim_cmp)st->cmp, got, st->want))
		return;
	record_failure(c, "T+%llu ms  %s[%s%s%s] %s %llu, got %llu -- %s",
		       (unsigned long long)sim_now_ms(c->s),
		       sim_metric_name(m),
		       st->a != NULL ? st->a : "all",
		       st->b != NULL ? "->" : "",
		       st->b != NULL ? st->b : "",
		       cmp_name((enum sim_cmp)st->cmp),
		       (unsigned long long)st->want,
		       (unsigned long long)got,
		       st->why != NULL ? st->why : "(no reason given)");
}

static void step_dump(struct sim_run_ctx *c)
{
	sim_dump_cluster(c->s, &c->out->dump,
			 c->step->why != NULL ? c->step->why : "snapshot");
}

static void step_end(struct sim_run_ctx *c)
{
	(void)c;
}

/* ------------------------------------------------------------------ *
 * The table
 * ------------------------------------------------------------------ */

typedef void (*sim_step_fn)(struct sim_run_ctx *);

static const sim_step_fn sim_step_table[SIM_STEP__COUNT] = {
	[SIM_STEP_END]            = step_end,
	[SIM_STEP_LINK]           = step_link,
	[SIM_STEP_LINK_PAIR]      = step_link_pair,
	[SIM_STEP_LINK_ALL]       = step_link_all,
	[SIM_STEP_CUT]            = step_cut,
	[SIM_STEP_NODE_LINK]      = step_node_link,
	[SIM_STEP_HALT]           = step_halt,
	[SIM_STEP_BOOT]           = step_boot,
	[SIM_STEP_RUN]            = step_run,
	[SIM_STEP_UNTIL_VCS]      = step_until_vcs,
	[SIM_STEP_UNTIL_CHANNELS] = step_until_channels,
	[SIM_STEP_SEND]           = step_send,
	[SIM_STEP_EXPECT]         = step_expect,
	[SIM_STEP_DUMP]           = step_dump
};

/* ------------------------------------------------------------------ *
 * Building the cluster a scenario declared
 * ------------------------------------------------------------------ */

static int build_nodes(struct sim_run_ctx *c)
{
	uint32_t i;

	for (i = 0; i < c->scn->n_nodes; i++) {
		const struct sim_node_decl *d = &c->scn->nodes[i];
		struct sim_node_cfg cfg;

		sim_node_cfg_default(&cfg, d->name, d->sysid, (uint8_t)i);
		if (d->have_override)
			sim_node_cfg_overlay(&cfg, &d->override);
		if (sim_add_node(c->s, &cfg) < 0) {
			record_failure(c, "cannot add node '%s' (%u max)",
				       d->name, SIM_MAX_NODES);
			return -1;
		}
	}
	if (sim_boot_all(c->s) < 0) {
		record_failure(c, "a declared node refused to boot");
		return -1;
	}
	return 0;
}

/* Two harness overflows would each turn a real stall into a green run, so
 * every scenario is checked for them whether it asked or not. */
static void check_harness_health(struct sim_run_ctx *c)
{
	c->out->checks += 3u;
	if (c->s->lan.queue_full != 0u)
		record_failure(c, "virtual LAN dropped %llu frames for want of "
				  "an in-flight slot (raise SIM_MAX_INFLIGHT)",
			       (unsigned long long)c->s->lan.queue_full);
	if (c->s->clock.overflows != 0u)
		record_failure(c, "timer wheel dropped %u arms (raise "
				  "SIM_MAX_TIMERS)", c->s->clock.overflows);
	if (c->s->event_cap_hit)
		record_failure(c, "the run hit SIM_MAX_EVENTS (%u): a livelock, "
				  "or a scenario that asked for too much",
			       SIM_MAX_EVENTS);
}

/* ------------------------------------------------------------------ *
 * Running
 * ------------------------------------------------------------------ */

int sim_scenario_run(const struct sim_scenario *scn, uint64_t seed,
		     struct sim_scenario_out *out)
{
	static struct sim_scenario_out scratch;   /* when the caller wants none */
	struct sim_run_ctx c;
	uint32_t i;

	if (out == NULL)
		out = &scratch;
	memset(out, 0, sizeof(*out));
	sim_dump_reset(&out->dump);

	c.scn = scn;
	c.s = &out->sim;
	c.out = out;
	c.step_index = 0u;
	c.step = NULL;

	sim_init(c.s, seed);
	if (build_nodes(&c) != 0)
		return (int)out->failures;

	for (i = 0; i < scn->n_steps; i++) {
		const struct sim_step *st = &scn->steps[i];
		sim_step_fn fn;

		if (st->kind >= SIM_STEP__COUNT)
			continue;
		c.step_index = i;
		c.step = st;
		fn = sim_step_table[st->kind];
		if (fn != NULL)
			fn(&c);
	}

	check_harness_health(&c);
	out->trace = c.s->trace;
	/* The final snapshot always, so a failing scenario's report carries the
	 * state it failed in without the author having to have asked. */
	c.step_index = scn->n_steps;
	sim_dump_cluster(c.s, &out->dump, "final");
	return (int)out->failures;
}

void sim_scenario_report(const struct sim_scenario *scn,
			 const struct sim_scenario_out *out, int verbose)
{
	uint32_t i;

	printf("scenario \"%s\": seed %llu, %u checks, %u failures, "
	       "T+%llu ms, trace %016llX\n",
	       scn->name, (unsigned long long)out->sim.seed, out->checks,
	       out->failures, (unsigned long long)out->sim.clock.now_ms,
	       (unsigned long long)out->trace);
	for (i = 0; i < out->n_recorded; i++)
		printf("  FAIL step %u: %s\n", out->failure[i].step,
		       out->failure[i].text);
	if (out->failures > out->n_recorded)
		printf("  ... and %u more failures\n",
		       out->failures - out->n_recorded);
	if (verbose || out->failures != 0u)
		sim_dump_print(&out->dump);
}
