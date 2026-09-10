/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cluster_node.c - the 2-node cluster-GENESIS rig's node driver (rd vms-f6b).
 *
 * WHAT IT IS. The smallest possible stand-in for STARTUP.EXE's cluster boot
 * step, for a guest that has no OVMX userland: it opens /dev/vms, issues
 * VMS_IOCTL_SYSGEN_LOAD with this node's SYSGEN parameters, issues
 * VMS_IOCTL_CLUSTER_START (which is what brings PEA0:/SCS/CNXMAN up INSIDE the
 * executive, on the real NIC, doing its own L2 I/O -- design SS3.5 step 2), and
 * then POLLS the executive for what it actually holds.
 *
 * WHAT IT IS NOT. It is not a cluster daemon and holds no cluster state of its
 * own. It never touches the wire, never parses a frame, never computes a CSID,
 * a member count or a quorum. The retired userspace SCS daemon (SCSD.EXE,
 * deleted by #1052 and forbidden by tests/qemu/test_no_scsd_image.sh) IS the
 * shape this deliberately is not: every value this program prints is a field it
 * READ BACK from the executive one line earlier (INV-6). If the executive does
 * not hold a value, this prints the honest "not learned" marker beside it and
 * never a placeholder -- a fabricated CSID is what bugchecked a real VAX.
 *
 * THREE INDEPENDENT EXECUTIVE READS, printed side by side so they can be
 * compared rather than trusted:
 *   VMS_IOCTL_CLUSTER_DIAG_CSB (row CLUB) -- the connection manager's CLUB
 *   VMS_IOCTL_CLUSTER_GETSYI              -- $GETSYI's own projection of it
 *   VMS_IOCTL_CLUSTER_MEMBER_GET          -- SHOW CLUSTER's CSB table walk
 *
 * Markers go to stdout; the rig's init points that at ttyS1 and the host-side
 * verdict greps them.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "vms_ioctl.h"

#define SS_NORMAL 1u

/* ==========================================================================
 * 1. This node's configuration -- SYSGEN parameters, nothing else
 * ========================================================================== */

struct node_cfg {
	const char *tag;          /* "A"/"B": which node of the rig this is   */
	const char *scsnode;      /* SCSNODE                                  */
	unsigned    scssystemid;  /* SCSSYSTEMID                              */
	unsigned    votes;        /* VOTES                                    */
	unsigned    expected_votes;
	unsigned    vaxcluster;   /* VAXCLUSTER                               */
	unsigned    group;        /* CLUSTER_AUTHORIZE group                  */
	unsigned    recnxinterval;/* RECNXINTERVAL, seconds                   */
	/*
	 * CLUSTER_CREDITS: how many receive buffers this port is asked to
	 * commit per circuit. It is LOAD-BEARING, not decoration. The port
	 * grants each circuit the smaller of this and the buffers the fork
	 * context really allocated, advertises exactly that at abs 95 of its
	 * formation body, and the PEER may then send that many messages and no
	 * more (p. 2-43: "no credit, no message"). A node that leaves it 0
	 * advertises a grant of 0, and its peer -- correctly, and counted as
	 * vc_credits_absent -- never puts a single sequenced frame on the
	 * circuit: the VC opens and nothing can travel on it.
	 */
	unsigned    cluster_credits;
	unsigned    window;       /* how long to poll, seconds                */
};

static void cfg_defaults(struct node_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->tag = "?";
	c->scsnode = "";
	c->vaxcluster = 2u;
	c->recnxinterval = 20u;
	c->cluster_credits = 32u;   /* VMS's own CLUSTER_CREDITS default */
	c->window = 90u;
}

/* One "--name=value" argument. Returns 0 if it was consumed. */
static int cfg_take(struct node_cfg *c, const char *arg)
{
	const char *v;

#define TAKE_STR(k, field) \
	if (strncmp(arg, "--" k "=", sizeof("--" k "=") - 1u) == 0) { \
		c->field = arg + sizeof("--" k "=") - 1u; return 0; }
#define TAKE_U(k, field) \
	if (strncmp(arg, "--" k "=", sizeof("--" k "=") - 1u) == 0) { \
		v = arg + sizeof("--" k "=") - 1u; \
		c->field = (unsigned)strtoul(v, NULL, 0); return 0; }

	TAKE_STR("tag", tag)
	TAKE_STR("scsnode", scsnode)
	TAKE_U("sysid", scssystemid)
	TAKE_U("votes", votes)
	TAKE_U("expected-votes", expected_votes)
	TAKE_U("vaxcluster", vaxcluster)
	TAKE_U("group", group)
	TAKE_U("recnx", recnxinterval)
	TAKE_U("credits", cluster_credits)
	TAKE_U("window", window)
#undef TAKE_U
#undef TAKE_STR
	return -1;
}

static int cfg_parse(struct node_cfg *c, int argc, char **argv)
{
	int i;

	cfg_defaults(c);
	for (i = 1; i < argc; i++) {
		if (cfg_take(c, argv[i]) != 0) {
			fprintf(stderr, "cluster_node: unknown argument %s\n",
				argv[i]);
			return -1;
		}
	}
	if (c->scsnode[0] == '\0' || c->scssystemid == 0u) {
		fprintf(stderr, "cluster_node: --scsnode and --sysid are "
				"required (SYSGEN identity)\n");
		return -1;
	}
	return 0;
}

/* ==========================================================================
 * 2. The two executive calls that CONFIGURE, in SYSBOOT's own order
 * ========================================================================== */

static int rig_open_executive(void)
{
	int fd = open("/dev/vms", O_RDWR);

	if (fd < 0)
		fprintf(stderr, "cluster_node: /dev/vms: %s\n", strerror(errno));
	return fd;
}

/*
 * STARTUP.EXE's own first executive call. Every ioctl except the four
 * DISPATCH-ALWAYS cluster diagnostics requires the caller to be a registered
 * VMS process (src/kernel/vms_module.c: vms_proc_find_or_err, -ESRCH
 * otherwise), because SYSGEN_LOAD and CLUSTER_START are executive operations a
 * process performs -- not anonymous pokes at a device node.
 */
static int rig_register(int fd, const struct node_cfg *c)
{
	struct vms_register_args a;

	memset(&a, 0, sizeof(a));
	if (ioctl(fd, VMS_IOCTL_REGISTER, &a) != 0) {
		fprintf(stderr, "cluster_node: REGISTER: %s\n", strerror(errno));
		return -1;
	}
	printf("RIG-%s-REGISTER status=%u vms_pid=0x%08x\n",
	       c->tag, (unsigned)a.status, (unsigned)a.vms_pid);
	fflush(stdout);
	return a.status == SS_NORMAL ? 0 : -1;
}

/* SCSNODE travels as a fixed 8-byte field plus its significant length, exactly
 * as STARTUP.EXE hands it down; nothing else in this program touches it. */
static void sysgen_set_scsnode(struct vms_sysgen_load_args *a, const char *name)
{
	size_t n = strlen(name);

	if (n > 6u)
		n = 6u;   /* VMS truncates SCSNODE to 6 characters */
	memcpy(a->scsnode, name, n);
	a->scsnode_len = (uint8_t)n;
}

static void sysgen_fill(struct vms_sysgen_load_args *a,
			const struct node_cfg *c)
{
	memset(a, 0, sizeof(*a));
	sysgen_set_scsnode(a, c->scsnode);
	a->scssystemid_lo = (uint32_t)c->scssystemid;
	a->votes          = (uint16_t)c->votes;
	a->expected_votes = (uint16_t)c->expected_votes;
	a->recnxinterval  = (uint16_t)c->recnxinterval;
	a->vaxcluster     = (uint8_t)c->vaxcluster;
	a->cluster_credits = (uint16_t)c->cluster_credits;
	a->auth_group     = (uint16_t)c->group;
	/* The software-identity token STARTUP.EXE would carry down from the
	 * userland SSOT. This rig has no OVMX userland, so it supplies none
	 * rather than inventing a version string: sw_version_len 0 is the
	 * executive's own honest "no token supplied" (vms_ioctl.h). */
}

static int rig_sysgen_load(int fd, const struct node_cfg *c)
{
	struct vms_sysgen_load_args a;

	sysgen_fill(&a, c);
	if (ioctl(fd, VMS_IOCTL_SYSGEN_LOAD, &a) != 0) {
		fprintf(stderr, "cluster_node: SYSGEN_LOAD: %s\n",
			strerror(errno));
		return -1;
	}
	printf("RIG-%s-SYSGEN status=%u scsnode=%s sysid=%u votes=%u "
	       "expected_votes=%u vaxcluster=%u group=%u recnx=%u credits=%u\n",
	       c->tag, (unsigned)a.status, c->scsnode, c->scssystemid,
	       c->votes, c->expected_votes, c->vaxcluster, c->group,
	       c->recnxinterval, c->cluster_credits);
	fflush(stdout);
	return a.status == SS_NORMAL ? 0 : -1;
}

static int rig_cluster_start(int fd, const struct node_cfg *c)
{
	struct vms_cluster_start_args a;

	memset(&a, 0, sizeof(a));
	if (ioctl(fd, VMS_IOCTL_CLUSTER_START, &a) != 0) {
		fprintf(stderr, "cluster_node: CLUSTER_START: %s\n",
			strerror(errno));
		return -1;
	}
	printf("RIG-%s-START status=%u port_up=%u state=%u\n",
	       c->tag, (unsigned)a.status, (unsigned)a.port_up,
	       (unsigned)a.cluster_state);
	fflush(stdout);
	return a.status == SS_NORMAL ? 0 : -1;
}

/* ==========================================================================
 * 3. The executive READS -- every printed value comes from one of these
 * ========================================================================== */

static int rig_read_club(int fd, struct vms_club_view_wire *out)
{
	struct vms_cluster_diag_csb_args a;

	memset(&a, 0, sizeof(a));
	a.row = VMS_CLUSTER_DIAG_CSB_CLUB;
	if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_CSB, &a) != 0)
		return -1;
	if (a.status != SS_NORMAL)
		return -1;
	*out = a.club;
	return 0;
}

static int rig_read_getsyi(int fd, struct vms_cluster_getsyi_args *out)
{
	memset(out, 0, sizeof(*out));
	if (ioctl(fd, VMS_IOCTL_CLUSTER_GETSYI, out) != 0)
		return -1;
	return out->status == SS_NORMAL ? 0 : -1;
}

/* The CSB table is 3848 bytes; keep it off the stack. */
static struct vms_cluster_member_get_args rig_members;

static int rig_read_members(int fd)
{
	memset(&rig_members, 0, sizeof(rig_members));
	if (ioctl(fd, VMS_IOCTL_CLUSTER_MEMBER_GET, &rig_members) != 0)
		return -1;
	return rig_members.status == SS_NORMAL ? 0 : -1;
}

/* ==========================================================================
 * 4. Rendering -- names for executive enums, and the honest-omission rule
 * ========================================================================== */

static const char *cluster_state_name(uint32_t s)
{
	switch (s) {
	case 0u: return "OFF";
	case 1u: return "PORT_UP";
	case 2u: return "JOINING";
	case 3u: return "MEMBER";
	case 4u: return "STANDALONE";
	default: return "?";
	}
}

/*
 * THE ROLE, DERIVED FROM EXECUTIVE STATE AND NOTHING ELSE.
 *
 * A founding node is the one whose CLUB names ITSELF as the coordinator:
 * cnxman_coord_found() mints this node's CSID and coord_claim_club() then
 * writes club->coordinator_csid = club->local_csid. A joiner that has been
 * admitted holds a CSID it learned from somebody else, so its coordinator CSID
 * differs. A node that holds no CSID at all is neither, and says so.
 *
 * The rig's CONFIGURATION (which node was given VOTES) is NOT consulted here:
 * that would be asserting the answer the run is supposed to measure.
 */
static const char *rig_role_name(const struct vms_club_view_wire *club)
{
	if (!club->local_csid_valid)
		return "none";
	if (club->coordinator_valid && club->coordinator_csid == club->local_csid)
		return "founder";
	if (club->coordinator_valid)
		return "joiner";
	return "member-no-coordinator";
}

/* A CSID the executive has not learned is printed as "-", never as 0: 0 would
 * read as "node zero" instead of "the cluster has not assigned one". */
static void csid_str(char *buf, size_t n, uint32_t csid, uint8_t valid)
{
	if (valid)
		snprintf(buf, n, "0x%08x", csid);
	else
		snprintf(buf, n, "-");
}

static void print_club_line(const struct node_cfg *c, unsigned elapsed,
			    const struct vms_club_view_wire *club)
{
	char local[16], coord[16];

	csid_str(local, sizeof(local), club->local_csid, club->local_csid_valid);
	csid_str(coord, sizeof(coord), club->coordinator_csid,
		 club->coordinator_valid);
	printf("RIG-%s-CLUB t=%us state=%s(%u) role=%s csid=%s coord=%s "
	       "nodes=%u cevotes=%u quorum=%u expected=%u epoch=%u "
	       "n_csb=%u transition=%u bitmap=0x%08x\n",
	       c->tag, elapsed, cluster_state_name(club->state),
	       (unsigned)club->state, rig_role_name(club), local, coord,
	       (unsigned)club->cluster_nodes, (unsigned)club->cevotes,
	       (unsigned)club->quorum, (unsigned)club->expected_votes,
	       (unsigned)club->epoch, (unsigned)rig_members.n_members,
	       (unsigned)club->transition_active, (unsigned)club->bitmap[0]);
}

static void print_getsyi_line(const struct node_cfg *c,
			      const struct vms_cluster_getsyi_args *syi)
{
	char csid[16];

	csid_str(csid, sizeof(csid), syi->node_csid, syi->node_csid_valid);
	printf("RIG-%s-GETSYI member=%u nodes=%u votes=%u quorum=%u csid=%s\n",
	       c->tag, (unsigned)syi->cluster_member,
	       (unsigned)syi->cluster_nodes, (unsigned)syi->cluster_votes,
	       (unsigned)syi->cluster_quorum, csid);
}

static void print_member_rows(const struct node_cfg *c)
{
	uint32_t i;

	for (i = 0; i < rig_members.n_members; i++) {
		const struct vms_cluster_member *m = &rig_members.members[i];
		char csid[16];

		csid_str(csid, sizeof(csid), m->csid, (uint8_t)(m->csid != 0u));
		printf("RIG-%s-CSB i=%u node=%-6s sysid=%u csid=%s state=%s\n",
		       c->tag, (unsigned)i,
		       m->scsnode[0] ? m->scsnode : "-",
		       (unsigned)m->sysid, csid, m->state);
	}
}

/* ==========================================================================
 * 5. The executive's own DIAGNOSTICS, dumped once at the end
 *
 * These are the two read-only projections the executive already keeps for
 * exactly this purpose: SCS's SDA `SHOW CONNECTIONS` equivalent, and the
 * connection manager's JOIN TRANSITION RING (integration note E69 -- the
 * executive has no console log, so a join that stalls at a point the wire does
 * not expose can only be read here). Raw ordinals are printed rather than
 * names: this rig has no business translating executive enums, and the
 * kernel-core headers that define them are where a reader should look.
 * ========================================================================== */

static void dump_port_row(const struct node_cfg *c,
			  const struct vms_pe_view_wire *p)
{
	printf("RIG-%s-PORT open=%u link=%u mtu=%u max_pktsz=%u channels=%u "
	       "vcs=%u rx=%u tx=%u tx_err=%u rx_drop_nobuf=%u "
	       "rx_drop_class=%u\n",
	       c->tag, (unsigned)p->port_open, (unsigned)p->link_up,
	       (unsigned)p->mtu, (unsigned)p->max_pktsz,
	       (unsigned)p->n_channels, (unsigned)p->n_vcs,
	       (unsigned)p->rx_frames, (unsigned)p->tx_frames,
	       (unsigned)p->tx_errors, (unsigned)p->rx_drops_nobuf,
	       (unsigned)p->rx_drops_badclass);
}

static void dump_channel_row(const struct node_cfg *c, uint32_t i,
			     const struct vms_pe_channel_view_wire *ch)
{
	printf("RIG-%s-CHAN i=%u mac=%02x:%02x:%02x:%02x:%02x:%02x state=%u "
	       "sysid_valid=%u sysid=%u hello_tx=%u hello_rx=%u pktsz=%u\n",
	       c->tag, (unsigned)i, ch->remote_mac[0], ch->remote_mac[1],
	       ch->remote_mac[2], ch->remote_mac[3], ch->remote_mac[4],
	       ch->remote_mac[5], (unsigned)ch->state,
	       (unsigned)ch->remote_sysid_valid, (unsigned)ch->remote_sysid_lo,
	       (unsigned)ch->hello_tx, (unsigned)ch->hello_rx,
	       (unsigned)ch->verified_pktsz);
}

static void dump_vc_row(const struct node_cfg *c, uint32_t i,
			const struct vms_pe_vc_view_wire *vc)
{
	printf("RIG-%s-VC i=%u peer_sysid=%u state=%u send_seq=%u recv_seq=%u "
	       "peer_recv_ack=%u unacked=%u retx=%u rx_gaps=%u "
	       "credits_send=%u credits_recv=%u down_reason=%u\n",
	       c->tag, (unsigned)i, (unsigned)vc->peer_sysid_lo,
	       (unsigned)vc->state, (unsigned)vc->send_seq,
	       (unsigned)vc->recv_seq, (unsigned)vc->peer_recv_ack,
	       (unsigned)vc->unacked, (unsigned)vc->retransmits,
	       (unsigned)vc->rx_gaps, (unsigned)vc->credits_send,
	       (unsigned)vc->credits_receive, (unsigned)vc->down_reason);
}

/* One PEA0: read: the port-wide row, then every channel row, then every VC
 * row. `row` selects which of the three real snapshots the executive fills. */
static void rig_dump_port(int fd, const struct node_cfg *c)
{
	struct vms_cluster_diag_port_args a;
	uint32_t i;

	memset(&a, 0, sizeof(a));
	a.row = VMS_CLUSTER_DIAG_PORT_ROW;
	if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_PORT, &a) != 0 ||
	    a.status != SS_NORMAL) {
		printf("RIG-%s-PORT unavailable\n", c->tag);
		return;
	}
	dump_port_row(c, &a.port);

	for (i = 0; i < 16u; i++) {
		memset(&a, 0, sizeof(a));
		a.row = VMS_CLUSTER_DIAG_PORT_CHANNEL;
		a.index = i;
		if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_PORT, &a) != 0 ||
		    a.status != SS_NORMAL)
			break;
		dump_channel_row(c, i, &a.channel);
	}
	for (i = 0; i < 16u; i++) {
		memset(&a, 0, sizeof(a));
		a.row = VMS_CLUSTER_DIAG_PORT_VC;
		a.index = i;
		if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_PORT, &a) != 0 ||
		    a.status != SS_NORMAL)
			break;
		dump_vc_row(c, i, &a.vc);
	}
	fflush(stdout);
}

static void dump_scs_totals(const struct vms_scs_view_wire *v)
{
	printf(" sbs=%u cdts=%u sysaps=%u dir_lookups_sent=%u "
	       "dir_lookups_served=%u credit_stalls=%u\n",
	       (unsigned)v->n_sbs, (unsigned)v->n_cdts, (unsigned)v->n_sysaps,
	       (unsigned)v->dir_lookups_sent, (unsigned)v->dir_lookups_served,
	       (unsigned)v->credit_stalls);
}

static void dump_cdt_row(const struct node_cfg *c, uint32_t i,
			 const struct vms_scs_cdt_view_wire *cdt)
{
	printf("RIG-%s-CDT i=%u local=%.16s remote=%.16s peer_sysid=%u "
	       "conid=0x%08x state=%u mtype=0x%02x send=%u recv=%u "
	       "tx=%u rx=%u\n",
	       c->tag, (unsigned)i, (const char *)cdt->local_name,
	       (const char *)cdt->remote_name, (unsigned)cdt->peer_sysid_lo,
	       (unsigned)cdt->local_conid, (unsigned)cdt->state,
	       (unsigned)cdt->msgtype, (unsigned)cdt->credit_send,
	       (unsigned)cdt->credit_receive, (unsigned)cdt->msgs_sent,
	       (unsigned)cdt->msgs_received);
}

static void rig_dump_conn(int fd, const struct node_cfg *c)
{
	struct vms_cluster_diag_conn_args a;
	uint32_t i;

	memset(&a, 0, sizeof(a));
	a.row = VMS_CLUSTER_DIAG_CONN_ROW;
	if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_CONN, &a) != 0 ||
	    a.status != SS_NORMAL) {
		printf("RIG-%s-SCS unavailable\n", c->tag);
		return;
	}
	printf("RIG-%s-SCS", c->tag);
	dump_scs_totals(&a.scs);

	for (i = 0; i < 32u; i++) {
		memset(&a, 0, sizeof(a));
		a.row = VMS_CLUSTER_DIAG_CONN_CDT;
		a.index = i;
		if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_CONN, &a) != 0 ||
		    a.status != SS_NORMAL)
			break;
		dump_cdt_row(c, i, &a.cdt);
	}
	fflush(stdout);
}

static void dump_join_rec(const struct node_cfg *c,
			  const struct cnxman_diag_rec_wire *r)
{
	printf("RIG-%s-JOINREC seq=%u t=%u rep=%u kind=%u state=%u->%u "
	       "event=%u detail=%u cat=0x%02x op=0x%02x rx=%u rc=%d aux=%u\n",
	       c->tag, (unsigned)r->seq, (unsigned)r->t_ms,
	       (unsigned)r->repeat, (unsigned)r->kind, (unsigned)r->state,
	       (unsigned)r->new_state, (unsigned)r->event, (unsigned)r->detail,
	       (unsigned)r->cat, (unsigned)r->op, (unsigned)r->rx,
	       (int)r->rc, (unsigned)r->aux);
}

/* The ring is walked by advancing `first` by however many rows the executive
 * actually returned; n_rows == 0 is the end. */
static void rig_dump_join(int fd, const struct node_cfg *c)
{
	static struct vms_cluster_diag_join_args a;
	uint32_t first = 0u, i, guard;

	for (guard = 0u; guard < 16u; guard++) {
		memset(&a, 0, sizeof(a));
		a.first = first;
		if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_JOIN, &a) != 0 ||
		    a.status != SS_NORMAL) {
			printf("RIG-%s-JOIN unavailable\n", c->tag);
			return;
		}
		if (first == 0u)
			printf("RIG-%s-JOIN state=%u failure=%u enabled=%u "
			       "count=%u recorded=%u ignored=%u\n",
			       c->tag, (unsigned)a.view.join_state,
			       (unsigned)a.view.join_failure,
			       (unsigned)a.view.enabled,
			       (unsigned)a.view.count,
			       (unsigned)a.view.recorded,
			       (unsigned)a.view.ignored_events);
		if (a.view.n_rows == 0u)
			break;
		for (i = 0; i < a.view.n_rows &&
			    i < VMS_CLUSTER_DIAG_JOIN_ROWS; i++)
			dump_join_rec(c, &a.view.row[i]);
		first += a.view.n_rows;
	}
	fflush(stdout);
}

/* ==========================================================================
 * 6. The poll loop
 * ========================================================================== */

struct rig_sample {
	struct vms_club_view_wire      club;
	struct vms_cluster_getsyi_args syi;
	int                            ok;
};

static void rig_sample_take(int fd, struct rig_sample *s)
{
	memset(s, 0, sizeof(*s));
	if (rig_read_club(fd, &s->club) != 0)
		return;
	if (rig_read_getsyi(fd, &s->syi) != 0)
		return;
	if (rig_read_members(fd) != 0)
		return;
	s->ok = 1;
}

static void rig_report(const struct node_cfg *c, unsigned elapsed,
		       const struct rig_sample *s)
{
	if (!s->ok) {
		printf("RIG-%s-CLUB t=%us READ-FAILED (the executive answered "
		       "no cluster state)\n", c->tag, elapsed);
		fflush(stdout);
		return;
	}
	print_club_line(c, elapsed, &s->club);
	print_getsyi_line(c, &s->syi);
	print_member_rows(c);
	fflush(stdout);
}

/*
 * The VERDICT LINE, and the two facts it is allowed to carry.
 *
 * `member` is cl->state == MEMBER as the CLUB reports it, cross-checked
 * against $GETSYI's own SYI$_CLUSTER_MEMBER; `cn` is the CLUB's own member
 * count (cnxman_club_recount_members(), the SELECTED-flag walk), cross-checked
 * against SYI$_CLUSTER_NODES. A disagreement between the two projections is
 * printed as a mismatch rather than resolved -- the rig has no business
 * choosing which executive read to believe.
 */
static void rig_verdict(const struct node_cfg *c, const struct rig_sample *s)
{
	char csid[16];
	int member, mismatch;

	if (!s->ok) {
		printf("RIG-%s-FINAL READ-FAILED\n", c->tag);
		fflush(stdout);
		return;
	}
	member = s->club.state == 3u;   /* VMS_CLUSTER_MEMBER */
	mismatch = (member != (s->syi.cluster_member != 0u)) ||
		   (s->club.cluster_nodes != s->syi.cluster_nodes);
	csid_str(csid, sizeof(csid), s->club.local_csid, s->club.local_csid_valid);

	printf("RIG-%s-FINAL role=%s member=%d state=%s csid=%s cn=%u "
	       "quorum=%u cevotes=%u epoch=%u projections=%s\n",
	       c->tag, rig_role_name(&s->club), member,
	       cluster_state_name(s->club.state), csid,
	       (unsigned)s->club.cluster_nodes, (unsigned)s->club.quorum,
	       (unsigned)s->club.cevotes, (unsigned)s->club.epoch,
	       mismatch ? "DISAGREE" : "agree");
	fflush(stdout);
}

static int rig_poll(int fd, const struct node_cfg *c)
{
	struct rig_sample s;
	unsigned t;

	for (t = 0; t < c->window; t++) {
		sleep(1);
		rig_sample_take(fd, &s);
		rig_report(c, t + 1u, &s);
	}
	rig_sample_take(fd, &s);
	rig_verdict(c, &s);
	rig_dump_port(fd, c);
	rig_dump_conn(fd, c);
	rig_dump_join(fd, c);
	return 0;
}

int main(int argc, char **argv)
{
	struct node_cfg cfg;
	int fd;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (cfg_parse(&cfg, argc, argv) != 0)
		return 2;

	fd = rig_open_executive();
	if (fd < 0)
		return 2;
	if (rig_register(fd, &cfg) != 0)
		return 2;
	if (rig_sysgen_load(fd, &cfg) != 0)
		return 2;
	if (rig_cluster_start(fd, &cfg) != 0)
		return 2;

	rig_poll(fd, &cfg);
	close(fd);
	return 0;
}
