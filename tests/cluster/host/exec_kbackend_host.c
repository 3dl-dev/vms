/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_kbackend_host.c - the host backend's one piece of state.
 *
 * `exec_host_interrupt_waits` is the INTERRUPT SEAM documented in
 * exec_kbackend_host.h: it has to be ONE object shared by the test TU and the
 * facility TU it drives, so it is defined here rather than in the header.
 * Default 0 -- every wait behaves exactly as it did before rd vms-f87 unless a
 * test asks otherwise, and no shipping code names it.
 */
unsigned int exec_host_interrupt_waits;
