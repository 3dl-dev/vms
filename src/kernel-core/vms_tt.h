/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_tt.h - the executive terminal CLASS driver (rd vms-f8c, epic vms-4eba).
 *
 * VMS splits the terminal driver in two: a CLASS driver (TTDRIVER) that owns
 * everything a terminal MEANS -- the type-ahead buffer, reads and their echo,
 * line editing, terminators, out-of-band characters -- and a PORT driver per
 * line that only moves bytes in and out (OpenVMS I/O User's Reference Manual,
 * "Terminal Driver"). OVMX keeps that split:
 *
 *   class driver   src/kernel-core/vms_tt.c   substrate-free; this header.
 *   port driver    the substrate tty, reached through a LINE DISCIPLINE the
 *                  executive registers -- Linux tty_ldisc_ops
 *                  (src/kernel/vms_tt_linux.c); NetBSD struct linesw (later).
 *
 * Attaching the line discipline REPLACES the substrate's own (n_tty): there is
 * no ECHO/ICANON left for anyone to toggle. Every received byte reaches
 * vms_tt_receive(); every byte the class driver emits leaves through the port's
 * xmit op.
 *
 * THE SEMANTICS (the faithful part; docs/design-terminal-driver.md):
 *   - a byte received while NO read is outstanding goes into the type-ahead
 *     buffer and is NOT echoed. Only the out-of-band characters act at once.
 *   - a read consumes type-ahead, then new input, echoing each character AS IT
 *     IS CONSUMED (never on receipt), applying the line-editing keys, and ends
 *     on a terminator (the read's mask, or the standard set), when the buffer
 *     fills, on its timeout, or on ^Z.
 *   - ^X purges the type-ahead buffer (and the line of a read in progress) the
 *     moment it is typed.
 *
 * Clean-room (AGENTS.md Rule 8): from the public I/O User's Reference and the
 * observed bytes of real VMS consoles (docs/oracle/keystroke/). No VSI/HPE
 * source or binary.
 */
#ifndef VMS_TT_H
#define VMS_TT_H

#include "exec_kbackend.h"
#include "vms_ioctl.h"

/* TTY_TYPAHDSZ / TTY_ALTYPAHD defaults (SYSGEN; OpenVMS System Management
 * Utilities Reference, "System Parameters"). */
#define VMS_TT_TYPAHDSZ       78
#define VMS_TT_ALTYPAHD       200
#define VMS_TT_TYPAHD_MAX     VMS_TT_ALTYPAHD
#define VMS_TT_LINE_MAX       1024   /* longest line one read assembles */
#define VMS_TT_PROMPT_MAX     512
#define VMS_TT_OBUF           2048   /* echo bytes queued for the port */

/* Read modifiers: the VMS_TT_RD_* flags of vms_ioctl.h (the IO$M_* meanings). */

struct vms_device;
struct vms_tt;

/* The port driver's side of the seam. xmit MUST NOT sleep: it is called from
 * the receive path (echo) as well as from process context. */
struct vms_tt_port_ops {
	void (*xmit)(void *port, const uint8_t *buf, size_t n);
	/* Process-context output: MAY sleep until the port has taken it all.
	 * Returns 0, or -EINTR if the writer was signalled. */
	int (*write)(void *port, const uint8_t *buf, size_t n);
	/* An out-of-band ^Y / ^C with no AST armed for it (rd vms-f0fb lands the
	 * ASTs); the port delivers it as the substrate's interrupt. May be NULL. */
	void (*interrupt)(void *port, uint8_t ch);
};

/* One completed (or failed) read. */
struct vms_tt_read_result {
	uint32_t status;     /* SS$_NORMAL / SS$_TIMEOUT / SS$_ABORT / ... */
	uint32_t count;      /* data bytes before the terminator */
	uint32_t term;       /* the terminator character (0 if none) */
	uint32_t termsz;     /* 1 when a terminator ended the read, else 0 */
};

struct vms_tt_read_req {
	uint32_t flags;              /* VMS_TT_RD_* */
	uint32_t bufsz;              /* bytes the caller can take */
	uint32_t timeout_s;          /* with VMS_TT_RD_TIMED */
	uint32_t termmask[8];        /* with VMS_TT_RD_TERMMASK: bit n = char n */
	const uint8_t *prompt;       /* kernel copy, or NULL */
	uint32_t promptsz;
};

/* Lifecycle: a terminal device row gains a class-driver instance when a port
 * attaches (the line discipline's bind), and loses it on detach. */
struct vms_tt *vms_tt_attach(struct vms_device *dev,
                             const struct vms_tt_port_ops *ops, void *port);
void vms_tt_detach(struct vms_tt *tt);

/* Port -> class: received bytes (any context the port's receive runs in). */
void vms_tt_receive(struct vms_tt *tt, const uint8_t *buf, size_t n);

/* A read (process context; may sleep). `out` gets up to req->bufsz bytes.
 * Returns 0 with *res filled, or -EINTR when the caller was signalled
 * (the read is then cancelled: res->status = SS$_ABORT). */
int vms_tt_read(struct vms_tt *tt, const struct vms_tt_read_req *req,
                uint8_t *out, struct vms_tt_read_result *res);

/* A write through the class driver (process context). LF -> CR LF when
 * `cooked` (a Unix-style writer through the port's own write path).
 * 0, or -EINTR. */
int vms_tt_write(struct vms_tt *tt, const uint8_t *buf, size_t n, int cooked);

/* PASSALL (IO$_SETMODE P2 = IO$K_TT_PASSALL): every byte is data, nothing is
 * echoed or edited, a read completes on whatever has arrived. */
void vms_tt_set_passall(struct vms_tt *tt, int on);

/* Does a read have something to return without blocking? (poll) */
int vms_tt_readable(struct vms_tt *tt);

/* The class-driver instance of a terminal row, REFERENCED, or NULL (no port
 * attached). Every non-NULL return is paired with vms_tt_release(). */
struct vms_tt *vms_tt_of(struct vms_device *dev);
void vms_tt_release(struct vms_tt *tt);

/* The /dev/vms surface (vms_module.c dispatch). */
struct vms_proc;
long vms_ioctl_tt_read(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_tt_write(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_tt_setmode(struct vms_proc *proc, unsigned long arg);

#endif /* VMS_TT_H */
