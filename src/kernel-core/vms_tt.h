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
/* The /dev/vms argument structs (struct vms_tt_*_args, VMS_TT_RD_*) come from
 * vms_internal.h: Linux vms_ioctl.h, NetBSD vms_tt_nb.h (byte-identical). */

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
	/* TTSYNC: hold (stop = 1, ^S) or release (stop = 0, ^Q) the line's
	 * output, at the port. May be NULL. Called from the receive path. */
	void (*flow)(void *port, int stop);
	/* The class driver's last reference is gone: free the port. May be
	 * NULL (a port that frees itself). Never called with a lock held. */
	void (*release)(void *port);
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
	const void *owner;           /* who may resume this read after a signal
				      * (vms_tt_read); NULL: not resumable */
	int ldisc;                   /* the substrate's read(2) on the line */
};

/* Lifecycle (vms_tt.c "LIFETIME"): vms_tt_bind creates the instance and
 * attaches it to a terminal row; vms_tt_detach (the port's close/hangup) ends
 * the attachment. `proc` (the process issuing the line's VMS_TTIOC_BIND) must
 * hold CMKRNL -- decided by the executive's protection code (vms_prot.h),
 * never by a substrate capability -- and `devnam` must name a terminal row with
 * no port yet. SS$_NORMAL with *out set, or SS$_NOPRIV / SS$_NOSUCHDEV /
 * SS$_DEVALLOC / SS$_INSFMEM. After vms_tt_detach the port's ops are never
 * called again, and its memory is released through ops->release when the last
 * reference goes. */
struct vms_proc;
uint32_t vms_tt_bind(struct vms_proc *proc, const char *devnam,
                     const struct vms_tt_port_ops *ops, void *port,
                     struct vms_tt **out);
void vms_tt_detach(struct vms_tt *tt);
/* The instance's line was re-opened (a console line discipline closed and
 * re-opened by a session hangup): continue on the new port, releasing the
 * old. The binding, type-ahead and outstanding reads survive. */
void vms_tt_set_port(struct vms_tt *tt, const struct vms_tt_port_ops *ops, void *port);
/* The line's session hung up: end read(2)-path reads (vms_tt.c). */
void vms_tt_kick_ldisc(struct vms_tt *tt);

/* Port -> class: received bytes (any context the port's receive runs in). */
void vms_tt_receive(struct vms_tt *tt, const uint8_t *buf, size_t n);

/* A read (process context; may sleep). `out` gets up to req->bufsz bytes.
 * Returns 0 with *res filled, or -ERESTARTSYS when the caller was signalled:
 * the read is then SUSPENDED, not ended, and the same req->owner calling
 * again resumes it (vms_tt.c). */
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
 * attached). Every non-NULL return -- and every vms_tt_get -- is paired with
 * vms_tt_release(). */
struct vms_tt *vms_tt_of(struct vms_device *dev);
void vms_tt_get(struct vms_tt *tt);
void vms_tt_release(struct vms_tt *tt);

/* The /dev/vms surface (vms_module.c dispatch). */
struct vms_proc;
long vms_ioctl_tt_read(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_tt_write(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_tt_setmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_tt_sense(struct vms_proc *proc, unsigned long arg);

#endif /* VMS_TT_H */
