/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pe_fake_ops.h - the injected struct pe_ops the FC-P0.8 channel-FSM host tests
 * drive the port with, plus a peer-station frame source.
 *
 * THE CLOCK IS A VARIABLE and THE WIRE IS AN ARRAY. Design SS3.9 rule 6:
 * deadlines are injected, "so a test drives time". Every test here sets
 * `f.now_ms` and calls; a twenty-second listen timeout and a four-rung
 * six-second packet-size ladder therefore run in microseconds and are exactly
 * reproducible. Every frame the FSM emits is captured verbatim and is asserted
 * on ONLY by re-parsing it through the codec -- a test that compared bytes at
 * hand-written offsets would be asserting the same arithmetic the code under
 * test does (design SS3.9 rule 2 applies to tests too).
 *
 * `alloc` and `free` are deliberately left NULL. The channel FSM owns a fixed
 * frame scratch buffer and has no business allocating; a NULL function pointer
 * is a harder assertion of that than any counter, because if it ever reaches
 * for one these tests crash instead of quietly passing.
 */
#ifndef OVMX_PE_FAKE_OPS_H
#define OVMX_PE_FAKE_OPS_H

#include <string.h>

#include "vms_cluster_codec.h"
#include "vms_cluster_codec_hello.h"
#include "vms_pe.h"
#include "vms_pe_fsm.h"

#define FAKE_PE_MAX_FRAMES 96

struct fake_pe_frame {
	uint32_t len;
	uint8_t  b[VMS_HELLO_PADDED_MAX_FRAME];
};

struct fake_pe {
	uint32_t now_ms;                /* the injected clock */
	uint32_t timers_armed;
	uint32_t timers_cancelled;
	uint32_t last_arm_ms;
	uint32_t last_arm_which;
	uint32_t logs;
	char     last_log[160];
	int      send_fails;            /* make ops->send report an error */
	uint32_t n_frames;
	uint32_t frames_dropped;        /* recorder full: a test bug, asserted 0 */
	struct fake_pe_frame frame[FAKE_PE_MAX_FRAMES];
};

static uint32_t fake_pe_now(void *ctx)
{
	return ((struct fake_pe *)ctx)->now_ms;
}

static int fake_pe_send(void *ctx, const uint8_t *frame, uint32_t len)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	if (f->send_fails)
		return 1;
	if (f->n_frames >= FAKE_PE_MAX_FRAMES || len > sizeof(f->frame[0].b)) {
		f->frames_dropped++;
		return 0;
	}
	f->frame[f->n_frames].len = len;
	memcpy(f->frame[f->n_frames].b, frame, len);
	f->n_frames++;
	return 0;
}

static void fake_pe_arm(void *ctx, enum pe_timer which, uint32_t key, uint32_t ms)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	(void)key;
	f->timers_armed++;
	f->last_arm_ms = ms;
	f->last_arm_which = (uint32_t)which;
}

static void fake_pe_cancel(void *ctx, enum pe_timer which, uint32_t key)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	(void)which;
	(void)key;
	f->timers_cancelled++;
}

static void fake_pe_log(void *ctx, const char *msg)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	f->logs++;
	if (msg != NULL) {
		size_t n = strlen(msg);

		if (n >= sizeof(f->last_log))
			n = sizeof(f->last_log) - 1;
		memcpy(f->last_log, msg, n);
		f->last_log[n] = '\0';
	}
}

static void fake_pe_ops_init(struct pe_ops *ops, struct fake_pe *f)
{
	memset(f, 0, sizeof(*f));
	memset(ops, 0, sizeof(*ops));
	ops->send = fake_pe_send;
	ops->arm_timer = fake_pe_arm;
	ops->cancel_timer = fake_pe_cancel;
	ops->now_ms = fake_pe_now;
	ops->log = fake_pe_log;
	/* ops->alloc and ops->free stay NULL -- see the file comment. */
	ops->ctx = f;
}

static void fake_pe_clear_frames(struct fake_pe *f)
{
	f->n_frames = 0;
}

/* ------------------------------------------------------------------ *
 * Reading an emitted frame back -- always through the codec
 * ------------------------------------------------------------------ */

struct fake_pe_decoded {
	struct vms_frame_info  fi;
	struct vms_hello_frame h;
	uint8_t                chan_word;
	uint32_t               len;
	int                    ok;
};

static struct fake_pe_decoded fake_pe_decode(const struct fake_pe *f,
					     uint32_t index)
{
	struct fake_pe_decoded d;

	memset(&d, 0, sizeof(d));
	if (index >= f->n_frames)
		return d;
	d.len = f->frame[index].len;
	if (vms_frame_classify(f->frame[index].b, d.len, &d.fi) != VMS_CODEC_OK)
		return d;
	if (vms_hello_parse(f->frame[index].b, d.len, &d.fi, &d.h) != VMS_CODEC_OK)
		return d;
	if (vms_sca_chan_word(f->frame[index].b, d.len, &d.fi,
			      &d.chan_word) != VMS_CODEC_OK)
		return d;
	d.ok = 1;
	return d;
}

/* How many emitted frames carry `word` at abs 30. The b2 count of the reference
 * joiner is ZERO, and that is a headline assertion of the replay test.
 * (`unused`: this header serves several test TUs and each uses a subset --
 * FC-P1.2's circuit tests drive the SCS family and never count a HELLO word.) */
static unsigned fake_pe_count_word(const struct fake_pe *f, uint8_t word)
	__attribute__((unused));
static unsigned fake_pe_count_word(const struct fake_pe *f, uint8_t word)
{
	unsigned i, n = 0;

	for (i = 0; i < f->n_frames; i++) {
		struct fake_pe_decoded d = fake_pe_decode(f, i);

		if (d.ok && d.chan_word == word)
			n++;
	}
	return n;
}

/* ------------------------------------------------------------------ *
 * A peer station, and the frames it puts on the wire
 *
 * Built with the SAME FC-P0.7 codec the FSM emits through, from a typed
 * identity -- so the stimulus is a real, classifiable SCA frame and never a
 * hand-laid byte array with offsets that could drift from the codec's.
 * ------------------------------------------------------------------ */

struct fake_peer {
	uint8_t  hw_mac[VMS_ETH_ADDR_LEN];
	uint8_t  lavc[VMS_ETH_ADDR_LEN];
	uint8_t  name[VMS_HELLO_NODENAME_MAX];
	uint8_t  name_len;
	uint8_t  nonce[VMS_DISC_NONCE_LEN];
	/* The sec 4(a).2 discovery-format span, abs 47-67. Zero by default so
	 * every existing scenario keeps the shape it had; a test that wants a
	 * peer to PRESENT it fills these, exactly as it fills `nonce`. */
	uint8_t  cap_span[VMS_DISC_CAPSPAN_LEN];
	uint8_t  reserved_64[VMS_DISC_RESERVED64_LEN];
	/*
	 * The sec 4(i).B incarnation THIS PEER ATTRIBUTES TO THE NODE UNDER
	 * TEST -- the number it advertises in its directed HELLO at payload
	 * [78:80] and then stamps at abs 36 on every frame of the circuit
	 * (E66). fake_peer_init sets 1, the fresh-contact value; a test that
	 * wants a member holding a residual for us raises it.
	 */
	uint16_t incarnation;
};

static void fake_peer_init(struct fake_peer *p, uint16_t sysid,
			   const uint8_t hw_mac[VMS_ETH_ADDR_LEN],
			   const char *name)
{
	size_t i;

	memset(p, 0, sizeof(*p));
	memcpy(p->hw_mac, hw_mac, VMS_ETH_ADDR_LEN);
	vms_cluster_lavc_addr_build(sysid, p->lavc);
	memset(p->name, ' ', sizeof(p->name));
	for (i = 0; i < VMS_HELLO_NODENAME_MAX && name[i] != '\0'; i++)
		p->name[i] = (uint8_t)name[i];
	p->name_len = VMS_HELLO_NODENAME_MAX;
	p->incarnation = 1u;    /* fresh contact, sec 4(i).B */
}

/*
 * One HELLO from the peer. `dst_hw`/`dst_lavc` are the addresses SS4(a).0
 * distinguishes: on a directed frame they are the target's HARDWARE MAC and its
 * cluster-LOGICAL address, and they are two different things.
 */
static uint32_t fake_peer_hello(const struct fake_peer *p,
				const uint8_t dst_hw[VMS_ETH_ADDR_LEN],
				const uint8_t dst_lavc[VMS_ETH_ADDR_LEN],
				uint8_t word, uint16_t incarnation,
				uint16_t padded_sca, uint8_t *out, uint32_t cap)
{
	struct vms_hello_frame h;
	uint32_t written = 0;

	memset(&h, 0, sizeof(h));
	memcpy(h.hdr.eth_dst, dst_hw, VMS_ETH_ADDR_LEN);
	memcpy(h.hdr.eth_src, p->hw_mac, VMS_ETH_ADDR_LEN);
	h.hdr.sca_len_field = (uint16_t)(VMS_HELLO_SCA_LEN - 2u);
	memcpy(h.hdr.dst_lavc, dst_lavc, VMS_ETH_ADDR_LEN);
	h.hdr.connect_flag = 0x0001u;
	memcpy(h.hdr.src_lavc, p->lavc, VMS_ETH_ADDR_LEN);
	h.hdr.word30 = (uint16_t)word;

	h.disc.namelen = p->name_len;
	memcpy(h.disc.name, p->name, VMS_HELLO_NODENAME_MAX);
	memcpy(h.disc.nonce, p->nonce, VMS_DISC_NONCE_LEN);
	memcpy(h.disc.cap_span, p->cap_span, VMS_DISC_CAPSPAN_LEN);
	memcpy(h.disc.reserved_64, p->reserved_64, VMS_DISC_RESERVED64_LEN);

	h.incarnation = incarnation;
	h.trailer_9205 = 0x0592u;
	memcpy(h.hw_mac, p->hw_mac, VMS_ETH_ADDR_LEN);
	h.trailer_2600 = 0x0026u;
	h.poller_sweep = (word == 0xa0u || word == 0xb1u) ? 0x0000u : 0x001fu;
	h.trailer_0064 = 0x0064u;

	if (padded_sca > VMS_HELLO_SCA_LEN) {
		if (vms_hello_build_padded(&h, padded_sca, out, cap,
					   &written) != VMS_CODEC_OK)
			return 0;
		return written;
	}
	if (vms_hello_build(&h, out, cap, &written) != VMS_CODEC_OK)
		return 0;
	return written;
}

/* A SS4(c) boot-time SOLICIT to the cluster group: a satellite asking to be
 * served a system disk. The only devspec length the spec grounds is 9
 * ("_$2$DUA0:"), so that is the one used. */
static uint32_t fake_peer_solicit(const struct fake_peer *p,
				  const uint8_t group[VMS_ETH_ADDR_LEN],
				  uint8_t *out, uint32_t cap)
	__attribute__((unused));
static uint32_t fake_peer_solicit(const struct fake_peer *p,
				  const uint8_t group[VMS_ETH_ADDR_LEN],
				  uint8_t *out, uint32_t cap)
{
	struct vms_solicit_frame s;
	uint32_t written = 0;

	memset(&s, 0, sizeof(s));
	memcpy(s.hdr.eth_dst, group, VMS_ETH_ADDR_LEN);
	memcpy(s.hdr.eth_src, p->hw_mac, VMS_ETH_ADDR_LEN);
	s.hdr.sca_len_field = 76u;            /* 78-byte SCA content, SS4(c) */
	memcpy(s.hdr.dst_lavc, group, VMS_ETH_ADDR_LEN);
	s.hdr.connect_flag = 0x0001u;
	memcpy(s.hdr.src_lavc, p->lavc, VMS_ETH_ADDR_LEN);
	s.hdr.word30 = 0x00b6u;               /* SS4(a): b6 on a SOLICIT */
	s.disc.namelen = p->name_len;
	memcpy(s.disc.name, p->name, VMS_HELLO_NODENAME_MAX);
	s.devspec_len = 9u;
	memcpy(s.devspec, "_$2$DUA0:", 9);

	if (vms_solicit_build(&s, out, cap, &written) != VMS_CODEC_OK)
		return 0;
	return written;
}

#endif /* OVMX_PE_FAKE_OPS_H */
