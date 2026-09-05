/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_dump.h - the simulator's SDA-like snapshot: the rung-2 analogue of the
 * SDA CSB/PORT readout the real-VAX lab reads by hand (FC-P1.4).
 *
 * WHY IT LOOKS LIKE SDA. Design §3.9's rung-2 row asks for "the simulator's
 * own SDA-like snapshots", and the reason is the same one that makes the
 * diagnostics ioctls mirror SDA (design decision 9): when a rung-5 lab run and
 * a rung-2 scenario disagree, the two readouts have to be COMPARABLE BY EYE.
 * The columns here are the columns SDA SHOW PORT / SHOW CLUSTER print, in the
 * order it prints them.
 *
 * EVERY NUMBER COMES FROM REAL FSM STATE. Where a frozen view struct exists --
 * `vms_pe_channel_view`, `vms_pe_vc_view` -- the dump goes through the SAME
 * projection the diagnostics ioctl uses (`pe_fsm_channel_project`,
 * `pe_fsm_vc_project`), so the rung-2 readout and the rung-4 ioctl readout are
 * literally the same struct filled by the same code. A value the executive
 * never learned prints as a blank, never as a zero that looks like a number
 * somebody claimed (INV-6).
 *
 * KNOWN GAP, FLAGGED NOT PATCHED: the PORT-level view (`vms_pe_view`:
 * port_open, mtu, tx_frames, rx_drops_*) is filled by `vms_pe_snapshot()` in
 * vms_pe.c, which is GLUE and is not part of the pure FSM this harness links.
 * There is no pure `pe_fsm_project(f, struct vms_pe_view *)` to call. The port
 * line below therefore reads the public `struct pe_fsm` counters directly and
 * says which they are. FC-P1.6 owns the glue; if it adds a pure projection,
 * this file should switch to it.
 *
 * THE OUTPUT IS DETERMINISTIC. No pointers, no wall-clock, no addresses: two
 * runs on one seed produce byte-identical text, which is one of the two
 * witnesses the determinism test compares.
 */
#ifndef OVMX_SIM_DUMP_H
#define OVMX_SIM_DUMP_H

#include <stdint.h>
#include <stddef.h>

struct sim;
struct sim_node;

/* A fixed sink, so a dump can be compared as well as printed. Overflow is
 * flagged rather than silently truncated. */
#define SIM_DUMP_MAX 65536

struct sim_dump {
	size_t len;
	int    overflow;
	char   b[SIM_DUMP_MAX];
};

void sim_dump_reset(struct sim_dump *d);

/* The whole simulated cluster: the LAN's own counters, then every node. */
void sim_dump_cluster(struct sim *s, struct sim_dump *d, const char *title);

/* One node: its port line, its channels, its circuits. */
void sim_dump_node(struct sim_node *n, struct sim_dump *d);

/* Write what has been dumped to a stream (a test prints it on failure). */
void sim_dump_print(const struct sim_dump *d);

#endif /* OVMX_SIM_DUMP_H */
