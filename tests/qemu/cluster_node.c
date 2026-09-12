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
	/*
	 * This node's OWN advertised software version -- the token STARTUP.EXE
	 * carries down from the userland SSOT and the port puts in its
	 * formation body (spec SS4(g)). It is this build declaring what it is,
	 * which is exactly what the field is for; it is NEVER a version read
	 * off a peer. rd vms-1ee: it is also the split-brain gate's trust
	 * anchor -- two nodes advertising the same token are provably the same
	 * implementation, and a real VAX advertising "VMS V7.3" is provably
	 * not.
	 */
	const char *swver;
	/*
	 * Run the CROSS-NODE phase after the membership window (section 6c).
	 * Off by default, so every existing mode of the rig behaves exactly as
	 * it did: the phase takes cluster-wide locks and makes the peer emit at
	 * this node, which has no business happening in a run measuring
	 * genesis.
	 */
	unsigned    xnode;
	/*
	 * Seconds to stay up AFTER this node's own cross-node work, before the
	 * survival verdict. It is what makes the last verdict line mean "still
	 * sane after the PEER'S frames landed" rather than "still sane after my
	 * own" -- the two nodes finish their phases at slightly different
	 * moments, and this is the overlap that covers the difference.
	 */
	unsigned    linger;
};

static void cfg_defaults(struct node_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->tag = "?";
	c->scsnode = "";
	c->vaxcluster = 2u;
	c->recnxinterval = 20u;
	c->cluster_credits = 32u;   /* VMS's own CLUSTER_CREDITS default */
	c->swver = "OVMX0.6";
	c->window = 90u;
	c->xnode = 0u;
	c->linger = 30u;
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
	TAKE_STR("swver", swver)
	TAKE_U("window", window)
	TAKE_U("xnode", xnode)
	TAKE_U("linger", linger)
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
	/*
	 * The software-identity token STARTUP.EXE carries down from the
	 * userland SSOT. This rig has no OVMX userland, so the token rides the
	 * kernel command line instead -- still THIS build declaring what it is,
	 * never a value read off a peer. Without it this node advertises
	 * nothing, and a peer cannot prove it is the same implementation: the
	 * split-brain gate (rd vms-1ee) then correctly refuses to build the
	 * directory at all.
	 */
	{
		size_t n = strlen(c->swver);

		if (n > sizeof(a->sw_version))
			n = sizeof(a->sw_version);
		memcpy(a->sw_version, c->swver, n);
		a->sw_version_len = (uint8_t)n;
	}
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
	       "expected_votes=%u vaxcluster=%u group=%u recnx=%u credits=%u "
	       "swver=%s\n",
	       c->tag, (unsigned)a.status, c->scsnode, c->scssystemid,
	       c->votes, c->expected_votes, c->vaxcluster, c->group,
	       c->recnxinterval, c->cluster_credits, c->swver);
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

/* ==========================================================================
 * 6b. THE LOCK PROBE (rd vms-1ee) -- what the DLM really does on a node that
 *     is a cluster member.
 *
 * Run on BOTH nodes, against the SAME resource name, AFTER membership has
 * settled. It measures three things and asserts none of them:
 *
 *   1. THE NON-REGRESSION THAT MATTERS MOST. Installing the DLM's wire arm
 *      puts the engine's requester ops in place for the first time. If the
 *      engine's directory resolver were installed with them, EVERY first $ENQ
 *      on a clustered node would return SS$_UNSUPPORTED -- the ACP's volume
 *      lock included -- and the node would stop mounting SYS$DISK. A granted
 *      lock here is that break not happening, measured on a real executive
 *      that is really a cluster member.
 *
 *   2. WHO MASTERS IT, read back from the lock database rather than assumed.
 *      With no groundable directory hash (see vms_dlm_scs.c) each member
 *      masters the name locally, and BOTH nodes reporting is_local_master=1
 *      for the same name is that honesty debt MEASURED instead of described.
 *
 *   3. WHETHER ANY LOCK IS HELD FOR A REMOTE CSID -- remote_holder_csid, the
 *      genuine cross-node-mastering readback. 0 on both nodes is the honest
 *      state of the cross-node path today.
 * ========================================================================== */
#define RIG_DLM_RESNAM "OVMX$DLMPROBE"

/*
 * One $ENQ at EX, ASYNC, with an optional blocking-AST routine.
 *
 * ASYNC IS NOT A CONVENIENCE. A SYNC ($ENQW, LCK_M_SYNC) request for a resource
 * mastered on the PEER sleeps inside the executive until the master's grant
 * arrives, and vms_lock.c's enq_wait_sync deliberately runs NO deadlock search
 * for a proxy LKB (the local wait-for graph cannot see the other node) -- so a
 * run in which the cross-node path did not work would HANG instead of reporting
 * what happened. Async returns with the request outstanding and leaves this
 * program to POLL the executive for the outcome, which is the only thing it is
 * ever allowed to print.
 */
static uint32_t rig_dlm_enq(int fd, const char *resnam, uint64_t blkastadr,
			    uint32_t *lkid_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = LCK_K_EXMODE;
	a.blkastadr = blkastadr;
	snprintf(a.resnam, sizeof(a.resnam), "%s", resnam);
	if (ioctl(fd, VMS_IOCTL_ENQ, &a) != 0)
		return 0u;
	*lkid_out = a.lkid;
	return a.status;
}

static uint32_t rig_dlm_deq(int fd, uint32_t lkid)
{
	struct vms_deq_args d;

	memset(&d, 0, sizeof(d));
	d.lkid = lkid;
	if (ioctl(fd, VMS_IOCTL_DEQ, &d) != 0)
		return 0u;
	return d.status;
}

static uint32_t rig_dlm_resmaster(int fd, const char *resnam,
				  struct vms_resmaster_args *rm)
{
	memset(rm, 0, sizeof(*rm));
	snprintf(rm->resnam, sizeof(rm->resnam), "%s", resnam);
	if (ioctl(fd, VMS_IOCTL_GET_RESMASTER, rm) != 0)
		return 0u;
	return rm->status;
}

static void rig_dlm_probe(int fd, const struct node_cfg *c)
{
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st;

	st = rig_dlm_enq(fd, RIG_DLM_RESNAM, 0u, &lkid);
	printf("RIG-%s-DLM-ENQ res=%s status=%u lkid=0x%08x\n",
	       c->tag, RIG_DLM_RESNAM, (unsigned)st, (unsigned)lkid);

	if (rig_dlm_resmaster(fd, RIG_DLM_RESNAM, &rm) != 0u)
		printf("RIG-%s-DLM-RES found=%u local_csid=0x%08x "
		       "master_csid=0x%08x is_local_master=%u dir_csid=0x%08x "
		       "n_granted=%u remote_holder_csid=0x%08x\n",
		       c->tag, (unsigned)rm.found, (unsigned)rm.local_csid,
		       (unsigned)rm.master_csid, (unsigned)rm.is_local_master,
		       (unsigned)rm.dir_csid, (unsigned)rm.n_granted,
		       (unsigned)rm.remote_holder_csid);
	else
		printf("RIG-%s-DLM-RES unavailable\n", c->tag);

	if (lkid != 0u)
		printf("RIG-%s-DLM-DEQ status=%u\n", c->tag,
		       (unsigned)rig_dlm_deq(fd, lkid));
	fflush(stdout);
}

/* ==========================================================================
 * 6c. THE CROSS-NODE PHASE (rd vms-94c; the emit half is vms-d7a3 / #1165)
 *
 * WHAT THE LOCAL PROBE ABOVE CANNOT REACH, AND WHY THIS EXISTS. The probe
 * enqueues ONE name on both nodes and both master it LOCALLY, so the arm's
 * cross-node emit paths -- the requester's op-0x03 $DEQ and the master's
 * op-0x04 BLOCKING AST -- are never entered. Rung A" (the OVMX-own directory
 * hash, src/kernel-core/vms_dlm_scs.c) is what changed that: in an
 * all-proven-OVMX cluster a root name's directory hash is grounded, the Lock
 * Directory Weight Vector resolves it to ONE member, and a name whose entry
 * names the PEER is genuinely mastered THERE.
 *
 * THE THREE THINGS THIS PHASE MEASURES, AND IT ASSERTS NONE OF THEM:
 *
 *   1. THAT A LOCK REALLY CROSSED. Not "a frame went out" -- that the
 *      executive's own lock database on this node reports the resource
 *      mastered by a CSID that is NOT this node's (master_csid != local_csid,
 *      is_local_master == 0), a value that reached the RSB only because the
 *      MASTER'S OWN GRANT carried it (vms_lock.c grant_recv). A local-mastered
 *      name reads exactly the opposite, which is what makes the readback a
 *      measurement rather than a decoration.
 *
 *   2. THAT THE ARM EMITTED. VMS_IOCTL_CLUSTER_DIAG_DLM projects the counters
 *      the running arm incremented AT THE MOMENT IT SENT, plus the connection
 *      manager's own independent count one layer down. A pcap proves a byte
 *      reached the segment; only these prove which executive put it there.
 *
 *   3. THAT THE RECEIVER ACTED, AND SURVIVED. Since rd vms-c72 the arm has a
 *      receive half for both opcodes, so the measurement is no longer a
 *      decline: the peer's own RIG-*-DLM-RECV line reports releases_received
 *      (a $DEQ that really released an LKB in ITS lock database) and its EMIT
 *      line reports blkasts_received/blkasts_delivered -- and the node goes on
 *      being a member. That is the never-crash-a-peer property taken against a
 *      live peer executive that ACTED on the frame rather than refusing it,
 *      which is the strictly harder claim.
 *
 * THE CANDIDATE NAMES ARE PER-NODE ("OVMXA$Xnn" on A, "OVMXB$Xnn" on B), and
 * that is load-bearing. With a shared set, both nodes would scan the SAME name
 * at the same moment, each taking an EX lock the other's request then blocks
 * on -- and the run would be measuring a race between two scans instead of the
 * routing. Disjoint sets remove the race WITHOUT telling either node where any
 * name routes: that is still discovered, name by name, from GET_RESMASTER.
 *
 * NOTHING HERE COMPUTES A HASH OR A ROUTE. This program does not know, and
 * must not know, which member the directory names for a name; it enqueues and
 * READS BACK. A rig that predicted the answer would pass on a build whose
 * routing was broken in exactly the way it predicted (Rule 8, INV-6).
 * ========================================================================== */

#define RIG_XN_CANDIDATES   16u    /* how many names to try before giving up  */
#define RIG_XN_GRANT_WAIT   40u    /* x RIG_XN_POLL_MS: the cross-node wait   */
#define RIG_XN_POLL_MS      250u
/*
 * The direct release proof's AFTER-poll window (6c below) MUST exceed
 * RIG_XN_GRANT_WAIT by a wide margin, not equal it. This node's own release
 * is sequenced AFTER its own watch-for-the-peer's-hold scan, which is itself
 * bounded by RIG_XN_GRANT_WAIT and, on a node with nothing to watch, runs to
 * that FULL timeout before returning -- so the PEER's own AFTER-poll can be
 * watching for a release that, structurally, may not fire until nearly
 * RIG_XN_GRANT_WAIT after that peer's own before-sample. A same-length AFTER
 * window is a coin flip on that skew (measured: it missed a real, already
 * wire-sent release by a slim margin on top of the 10s timeout). This is
 * rig-scheduling patience, not a looser correctness bar -- the assertion (an
 * actual counter rise, backed by an actual state re-read) is unchanged.
 */
#define RIG_XN_CLEAN_AFTER_WAIT 160u  /* x RIG_XN_POLL_MS = 40s */

struct rig_xnode {
	char     name[32];        /* the peer-mastered name, or "" if none    */
	uint32_t tried;           /* candidates enqueued before one crossed   */
	uint32_t local_csid;      /* THIS node, as the executive reported it  */
	uint32_t master_csid;     /* the PEER that masters `name`             */
	uint32_t dir_csid;        /* the directory node the vector named      */
	uint32_t hold_lkid;       /* the granted cross-node lock              */
	uint32_t hold_mode;       /* ...at the mode the executive granted     */
	uint32_t contend_lkid;    /* the second, incompatible request         */
	int      found;
};

/*
 * THE BLOCKING-AST ROUTINE this node registers on its cross-node $ENQ.
 *
 * A real address of a real function in this image, because that is what $ENQ's
 * blkastadr IS: the routine the executive calls when a lock this process holds
 * begins to block somebody. The executive stores it on the proxy LKB, and if
 * the arm's receive half ever consumes the master's op-0x04 it queues a genuine
 * user-mode AST carrying it (vms_lock.c vms_lock_dlm_xnode_blkast_recv), which
 * this program then drains with VMS_IOCTL_DELIVERAST and prints. So a delivery,
 * if one happens, is proven by the executive HANDING BACK THIS ROUTINE'S OWN
 * ADDRESS -- not by a counter this program chose to believe.
 *
 * It is never called: this rig has no AST dispatcher. Registering 0 instead
 * would make the delivery path decline honestly and prove nothing either way,
 * so a real address is registered and its arrival (or absence) is measured.
 */
static void rig_blkast_routine(unsigned long prm)
{
	(void)prm;
}

static void rig_msleep(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

/* This node's own candidate name for index `i`. See the per-node note above. */
static void rig_xn_name(const struct node_cfg *c, unsigned i, char *out,
			size_t n)
{
	snprintf(out, n, "OVMX%s$X%02u", c->tag, i);
}

/*
 * Is this name mastered by a node that is NOT us? The executive's own answer
 * and nothing else: a master CSID it genuinely holds, that differs from the
 * local one. `master_csid == 0` is UNMASTERED, which is a different fact from
 * "the peer masters it" -- conflating them is precisely the fabrication the
 * readback exists to prevent.
 */
static int rig_xn_is_peer_mastered(const struct vms_resmaster_args *rm)
{
	return rm->found != 0u && rm->local_csid != 0u &&
	       rm->master_csid != 0u &&
	       rm->master_csid != rm->local_csid &&
	       rm->is_local_master == 0u;
}

/*
 * Poll GET_RESMASTER until the executive holds a MASTER for this name, or the
 * deadline passes. The wait is the cross-node ROUND TRIP, not a timer: a
 * master CSID reaches this node's resource block only when the master's own
 * grant reply lands. A name this node masters itself answers on the first read.
 */
static void rig_xn_wait_master(int fd, const char *name,
			       struct vms_resmaster_args *rm)
{
	unsigned t;

	for (t = 0; t < RIG_XN_GRANT_WAIT; t++) {
		if (rig_dlm_resmaster(fd, name, rm) != 0u &&
		    rm->master_csid != 0u)
			return;
		rig_msleep(RIG_XN_POLL_MS);
	}
	(void)rig_dlm_resmaster(fd, name, rm);
}

/* The granted mode the executive holds for a lock ($GETLKI), or 0 if it has
 * no such lock. Never inferred from the $ENQ that asked for it: an async
 * request reports the mode REQUESTED, and the two differ until the grant. */
static uint32_t rig_xn_granted_mode(int fd, uint32_t lkid)
{
	struct vms_getlki_args g;

	memset(&g, 0, sizeof(g));
	g.lkid = lkid;
	if (ioctl(fd, VMS_IOCTL_GETLKI, &g) != 0 || g.status != SS_NORMAL)
		return 0u;
	return g.granted_mode;
}

static void rig_xn_print_res(const struct node_cfg *c, const char *what,
			     const char *name,
			     const struct vms_resmaster_args *rm)
{
	printf("RIG-%s-XN-%s res=%s found=%u local_csid=0x%08x "
	       "dir_csid=0x%08x master_csid=0x%08x is_local_master=%u "
	       "n_granted=%u remote_holder_csid=0x%08x\n",
	       c->tag, what, name, (unsigned)rm->found,
	       (unsigned)rm->local_csid, (unsigned)rm->dir_csid,
	       (unsigned)rm->master_csid, (unsigned)rm->is_local_master,
	       (unsigned)rm->n_granted, (unsigned)rm->remote_holder_csid);
	fflush(stdout);
}

/*
 * Try ONE candidate: $ENQ it EX (async, with a blocking-AST routine), wait for
 * the executive to hold a master, and classify from what it holds.
 *
 * A name this node masters itself is RELEASED again before the next candidate,
 * so the scan leaves no lock behind on a resource it is not using -- and so
 * the peer's own scan, which never touches these names, cannot be affected by
 * this one at all. Returns 1 when the candidate is the cross-node subject.
 */
static int rig_xn_try(int fd, const struct node_cfg *c, unsigned i,
		      struct rig_xnode *xn)
{
	struct vms_resmaster_args rm;
	char name[32];
	uint32_t lkid = 0u, st;

	rig_xn_name(c, i, name, sizeof(name));
	st = rig_dlm_enq(fd, name, (uint64_t)(uintptr_t)rig_blkast_routine,
			 &lkid);
	xn->tried++;
	if (st != SS_NORMAL || lkid == 0u) {
		printf("RIG-%s-XN-ENQ res=%s status=%u lkid=0x%08x "
		       "(not enqueued -- the executive refused, honestly)\n",
		       c->tag, name, (unsigned)st, (unsigned)lkid);
		fflush(stdout);
		return 0;
	}

	rig_xn_wait_master(fd, name, &rm);
	if (!rig_xn_is_peer_mastered(&rm)) {
		rig_xn_print_res(c, "LOCAL", name, &rm);
		(void)rig_dlm_deq(fd, lkid);
		return 0;
	}

	snprintf(xn->name, sizeof(xn->name), "%s", name);
	xn->local_csid  = rm.local_csid;
	xn->master_csid = rm.master_csid;
	xn->dir_csid    = rm.dir_csid;
	xn->hold_lkid   = lkid;
	xn->hold_mode   = rig_xn_granted_mode(fd, lkid);
	xn->found       = 1;
	rig_xn_print_res(c, "PEER", name, &rm);
	printf("RIG-%s-XN-HOLD res=%s lkid=0x%08x granted_mode=%u "
	       "master_csid=0x%08x tried=%u\n",
	       c->tag, xn->name, (unsigned)xn->hold_lkid,
	       (unsigned)xn->hold_mode, (unsigned)xn->master_csid,
	       (unsigned)xn->tried);
	fflush(stdout);
	return 1;
}

/* Scan this node's own candidate set for one the PEER masters. */
static void rig_xn_find(int fd, const struct node_cfg *c, struct rig_xnode *xn)
{
	unsigned i;

	memset(xn, 0, sizeof(*xn));
	for (i = 0; i < RIG_XN_CANDIDATES; i++)
		if (rig_xn_try(fd, c, i, xn))
			return;
	printf("RIG-%s-XN-HOLD NONE (no candidate of this node's own %u names "
	       "was mastered by the peer -- the directory resolved every one "
	       "to this node, or the vector is not usable)\n",
	       c->tag, (unsigned)RIG_XN_CANDIDATES);
	fflush(stdout);
}

/*
 * THE CONTENDER -- what makes the MASTER owe a BLOCKING AST.
 *
 * A second $ENQ at EX on the SAME name. It is a second cross-node request from
 * this node to the master, and at the master it meets a granted EX held FOR
 * THIS NODE'S CSID: incompatible, so the master QUEUES it and its arm sends an
 * op-0x04 to the holder -- which is this node (vms_lock.c's cross-node BLKAST
 * directive, vms_dlm_scs.c dlm_arm_send_blkast).
 *
 * WHY BOTH SIDES OF THE CONFLICT ARE THIS NODE. On a TWO-node cluster the
 * master's remote holder and its remote contender can only be the same peer:
 * the master itself is the other node. They are nonetheless two DIFFERENT lock
 * blocks with two different handles, which is exactly the conflict a BLKAST
 * exists for -- VMS notifies a HOLDER, not a node.
 *
 * NO LCK_M_NOQUEUE: the request must QUEUE, because "queued" is the master-side
 * state that owes the AST. It is never granted (the holder is this program and
 * it does not release first), which is expected and is why the $ENQ is async.
 */
static void rig_xn_contend(int fd, const struct node_cfg *c,
			   struct rig_xnode *xn)
{
	struct vms_resmaster_args rm;
	uint32_t lkid = 0u, st;

	if (!xn->found)
		return;
	st = rig_dlm_enq(fd, xn->name, (uint64_t)(uintptr_t)rig_blkast_routine,
			 &lkid);
	xn->contend_lkid = lkid;
	printf("RIG-%s-XN-CONTEND res=%s status=%u lkid=0x%08x mode=EX "
	       "(a second, incompatible cross-node request at the master)\n",
	       c->tag, xn->name, (unsigned)st, (unsigned)lkid);
	fflush(stdout);

	rig_msleep(3000u);   /* let the master's op-0x04 make the round trip */
	if (rig_dlm_resmaster(fd, xn->name, &rm) != 0u)
		rig_xn_print_res(c, "AFTER-CONTEND", xn->name, &rm);
	printf("RIG-%s-XN-CONTEND-MODE lkid=0x%08x granted_mode=%u "
	       "(0/NL = still queued at the master, which is the state that "
	       "owes the blocking AST)\n",
	       c->tag, (unsigned)lkid, (unsigned)rig_xn_granted_mode(fd, lkid));
	fflush(stdout);
}

/*
 * Drain this process's user-mode AST queue and print every AST the executive
 * hands back. A blocking AST that was really delivered shows up here carrying
 * rig_blkast_routine's own address; nothing else in this program queues one.
 *
 * An EMPTY queue is printed as such and is NOT a failure of this function: the
 * arm's op-0x04 RECEIVE half is a counted gap today (vms_dlm_scs.c), so the
 * honest expectation is zero, and the value of draining anyway is that the day
 * the receive half lands, this same rig measures the delivery.
 */
static unsigned rig_xn_drain_asts(int fd, const struct node_cfg *c)
{
	struct vms_ast_args a;
	unsigned n = 0, guard;

	for (guard = 0; guard < 16u; guard++) {
		memset(&a, 0, sizeof(a));
		if (ioctl(fd, VMS_IOCTL_DELIVERAST, &a) != 0)
			break;
		if (a.status != SS_NORMAL || a.astadr == 0u)
			break;
		printf("RIG-%s-XN-AST astadr=0x%llx astprm=0x%llx acmode=%u "
		       "is_blkast_routine=%d\n",
		       c->tag, (unsigned long long)a.astadr,
		       (unsigned long long)a.astprm, (unsigned)a.acmode,
		       a.astadr == (uint64_t)(uintptr_t)rig_blkast_routine);
		n++;
	}
	printf("RIG-%s-XN-ASTS delivered=%u\n", c->tag, n);
	fflush(stdout);
	return n;
}

/*
 * THE RELEASE -- what puts a real op-0x03 $DEQ on the wire.
 *
 * The HOLDER is released first and it is the one that crosses: it holds a
 * MASTER HANDLE (the lock id the master's own grant reply assigned), and the
 * codec refuses to build a $DEQ without one -- the fc8540ae INVLOCKID lesson.
 * The CONTENDER is released after, and it deliberately does NOT cross: it was
 * never granted, so this node holds no master handle for it, so the arm counts
 * `releases_no_wire_op` and sends nothing. Both outcomes are printed, because
 * the refusal is as much a measurement as the emission.
 */
static void rig_xn_release(int fd, const struct node_cfg *c,
			   struct rig_xnode *xn)
{
	if (!xn->found)
		return;
	if (xn->hold_lkid != 0u)
		printf("RIG-%s-XN-DEQ-HOLD res=%s lkid=0x%08x status=%u "
		       "(this is the $DEQ that must cross as op-0x03)\n",
		       c->tag, xn->name, (unsigned)xn->hold_lkid,
		       (unsigned)rig_dlm_deq(fd, xn->hold_lkid));
	if (xn->contend_lkid != 0u)
		printf("RIG-%s-XN-DEQ-CONTEND lkid=0x%08x status=%u "
		       "(no master handle: the arm must refuse the wire op and "
		       "count it, not invent one)\n",
		       c->tag, (unsigned)xn->contend_lkid,
		       (unsigned)rig_dlm_deq(fd, xn->contend_lkid));
	fflush(stdout);
}

/* The arm's own emit ledger, read back from the executive (INV-6). `phase`
 * names WHEN the reading was taken so two of them can be differenced. */
static void rig_dump_dlm(int fd, const struct node_cfg *c, const char *phase)
{
	struct vms_cluster_diag_dlm_args a;
	const struct vms_dlm_scs_view_wire *v = &a.dlm;

	memset(&a, 0, sizeof(a));
	if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_DLM, &a) != 0 ||
	    a.status != SS_NORMAL) {
		printf("RIG-%s-DLM at=%s unavailable status=%u "
		       "(the executive holds no DLM wire arm)\n",
		       c->tag, phase, (unsigned)a.status);
		fflush(stdout);
		return;
	}
	printf("RIG-%s-DLM at=%s connected=%u lockdirwt=%u gen=%u "
	       "proxy_lkbs=%u req_sent=%u req_received=%u grants_sent=%u "
	       "grants_received=%u declined=%u\n",
	       c->tag, phase, (unsigned)v->connected, (unsigned)v->lockdirwt,
	       (unsigned)v->rebuild_generation, (unsigned)v->proxy_lkbs,
	       (unsigned)v->req_sent, (unsigned)v->req_received,
	       (unsigned)v->grants_sent, (unsigned)v->grants_received,
	       (unsigned)v->declined);
	printf("RIG-%s-DLM-EMIT at=%s releases_sent=%u releases_no_wire_op=%u "
	       "blkasts_sent=%u blkasts_no_wire_op=%u blkasts_received=%u "
	       "blkasts_delivered=%u queued_no_reply=%u unparsed=%u "
	       "foreign_refused=%u\n",
	       c->tag, phase, (unsigned)v->releases_sent,
	       (unsigned)v->releases_no_wire_op, (unsigned)v->blkasts_sent,
	       (unsigned)v->blkasts_no_wire_op, (unsigned)v->blkasts_received,
	       (unsigned)v->blkasts_delivered, (unsigned)v->queued_no_reply,
	       (unsigned)v->unparsed, (unsigned)v->foreign_refused);
	/*
	 * THE RECEIVE LEDGER (rd vms-c72), on its own line because it is the
	 * half the emitting node's counters and a pcap both CANNOT show: what a
	 * PEER's op-0x03/op-0x04 did to THIS executive's lock database. A rig
	 * run where releases_sent rose on one node and releases_received stayed
	 * 0 on the other is precisely the gap this item closed, and it has to be
	 * readable as a number rather than inferred.
	 *
	 * `valblk_writes_received` (rd vms-727) sits right beside it: a peer's
	 * op-0x06 CONVERT-with-VALBLK that this executive's master-side apply
	 * (vms_lock_dlm_master_apply_valblk) really wrote into a resource block
	 * it masters -- the receive-ledger fact neither a pcap nor the sender's
	 * own emit counters can show.
	 */
	printf("RIG-%s-DLM-RECV at=%s releases_received=%u "
	       "valblk_writes_received=%u releases_refused=%u "
	       "blkasts_unparsed=%u deferred_grants_owed=%u\n",
	       c->tag, phase, (unsigned)v->releases_received,
	       (unsigned)v->valblk_writes_received,
	       (unsigned)v->releases_refused, (unsigned)v->blkasts_unparsed,
	       (unsigned)v->deferred_grants_owed);
	printf("RIG-%s-DLM-LEG at=%s sends=%u sends_refused=%u frames_rx=%u "
	       "replies_sent=%u declined=%u\n",
	       c->tag, phase, (unsigned)v->leg_sends,
	       (unsigned)v->leg_sends_refused, (unsigned)v->leg_frames_rx,
	       (unsigned)v->leg_replies_sent, (unsigned)v->leg_declined);
	/*
	 * The post path's four endings. Printed on its own line because when a
	 * frame does NOT go out this is the line that says WHICH silence it
	 * was -- and a proof that cannot distinguish "the queue refused" from
	 * "the lock was gone by the time the fork thread rebuilt the request"
	 * is a proof that will be read as the wrong one.
	 */
	printf("RIG-%s-DLM-POST at=%s queued=%u unqueued=%u lock_gone=%u "
	       "refused=%u\n",
	       c->tag, phase, (unsigned)v->posts_queued,
	       (unsigned)v->posts_unqueued, (unsigned)v->posts_lock_gone,
	       (unsigned)v->posts_refused);
	fflush(stdout);
}

/*
 * The phase, in the order the frames have to happen in: find a peer-mastered
 * name and hold it, contend to make the master owe a BLKAST, then release the
 * holder so a $DEQ crosses the other way. The ledger is read before and after
 * so the run reports a DIFFERENCE rather than a total.
 */
static void rig_xnode_phase(int fd, const struct node_cfg *c,
			    struct rig_xnode *xn)
{
	rig_dump_dlm(fd, c, "before");
	rig_xn_find(fd, c, xn);
	if (!xn->found) {
		rig_dump_dlm(fd, c, "after");
		return;
	}
	rig_xn_contend(fd, c, xn);
	(void)rig_xn_drain_asts(fd, c);
	rig_xn_release(fd, c, xn);
	rig_msleep(2000u);       /* let this node's own op-0x03 land */
	rig_dump_dlm(fd, c, "after");
}

/* ==========================================================================
 * 6c. THE DIRECT MASTER-SIDE RELEASE PROOF (rd vms-c72 conductor gap-close).
 *
 * The prior attempt bracketed the WRONG event with a MUDDIED resource: it
 * sampled GET_RESMASTER around whichever node's OWN release happened to run
 * next, on the SAME contended name (`xn`) that also carries a second,
 * incompatible request from the holder itself (rig_xn_contend) -- so a
 * released grant can be immediately refilled by that second request's
 * deferred grant, and n_granted never moves even though the specific LKB
 * that held it really left.
 *
 * THE FIX is two changes, both required:
 *
 *   1. A DEDICATED, SINGLE-HOLDER resource, in its own candidate namespace
 *      ("OVMX<tag>$R%02u", disjoint from the contended "$X" series) that
 *      NEVER gets a second $ENQ. n_granted on it can only ever be 0 or 1,
 *      so a 1->0 delta can only mean the one holder released.
 *
 *   2. THE RIGHT BRACKET. BEFORE is sampled once this node has confirmed the
 *      PEER already holds a granted lock on a name in ITS OWN candidate
 *      series that resolved to this node as master -- i.e. found via a
 *      read-only scan of the peer's namespace (Rule 8: no hash is computed,
 *      only read back). AFTER is sampled only once THIS node's own
 *      `releases_received` counter has RISEN past the baseline taken right
 *      after BEFORE -- i.e. only once this executive has actually PROCESSED
 *      the peer's op-0x03 for some release. Sequencing the RELEASE of this
 *      node's own held candidate strictly BEFORE the wait-for-rise poll (not
 *      before the BEFORE sample, which only discovers the peer's resource
 *      and never waits on a release) is what avoids the two-node
 *      deadlock-to-timeout a naive "wait first" ordering produces.
 *
 * Two independent roles run in the SAME process, because a 2-node rig cannot
 * know in advance which name routes which way:
 *   HOLDER role:  own $R-candidate the peer masters -> hold, then release.
 *   WATCHER role: peer's $R-namespace, a candidate THIS node masters -> the
 *                 BEFORE/AFTER delta lives here.
 * Both roles are symmetric across the two nodes, exactly like the xnode
 * phase's own find/contend: which node ends up holding which resource is
 * discovered from the executive, never assumed.
 * ========================================================================== */

/* This node's own candidate name for the dedicated, single-holder series. */
static void rig_xn_clean_name(const struct node_cfg *c, unsigned i, char *out,
			      size_t n)
{
	snprintf(out, n, "OVMX%s$R%02u", c->tag, i);
}

struct rig_xn_clean_holder {
	char     name[32];   /* this node's own held candidate ("" if none) */
	uint32_t lkid;
	int      held;
};

/*
 * HOLDER role: scan this node's own $R-series (same discovery method as
 * rig_xn_try -- GET_RESMASTER readback, Rule 8) for one the PEER masters,
 * and hold it with NO second $ENQ, so it is a genuine single-holder
 * resource. Candidates this node masters itself are released immediately,
 * same as rig_xn_try.
 */
static void rig_xn_clean_hold(int fd, const struct node_cfg *c,
			      struct rig_xn_clean_holder *h)
{
	struct vms_resmaster_args rm;
	unsigned i;

	memset(h, 0, sizeof(*h));
	for (i = 0; i < RIG_XN_CANDIDATES; i++) {
		char name[32];
		uint32_t lkid = 0u, st;

		rig_xn_clean_name(c, i, name, sizeof(name));
		st = rig_dlm_enq(fd, name, 0u, &lkid);
		if (st != SS_NORMAL || lkid == 0u)
			continue;
		rig_xn_wait_master(fd, name, &rm);
		if (!rig_xn_is_peer_mastered(&rm)) {
			(void)rig_dlm_deq(fd, lkid);
			continue;
		}
		snprintf(h->name, sizeof(h->name), "%s", name);
		h->lkid = lkid;
		h->held = 1;
		printf("RIG-%s-RESCLEAN-HOLD res=%s lkid=0x%08x master_csid=0x%08x "
		       "(single holder, no contender -- the direct-release "
		       "subject)\n",
		       c->tag, h->name, (unsigned)h->lkid,
		       (unsigned)rm.master_csid);
		fflush(stdout);
		return;
	}
	printf("RIG-%s-RESCLEAN-HOLD NONE (no dedicated candidate was mastered "
	       "by the peer)\n", c->tag);
	fflush(stdout);
}

/* Release the dedicated resource this node holds -- the $DEQ that must cross
 * as op-0x03 with no contender queued behind it. */
static void rig_xn_clean_release(int fd, const struct node_cfg *c,
				 const struct rig_xn_clean_holder *h)
{
	if (!h->held)
		return;
	printf("RIG-%s-RESCLEAN-DEQ res=%s lkid=0x%08x status=%u\n",
	       c->tag, h->name, (unsigned)h->lkid,
	       (unsigned)rig_dlm_deq(fd, h->lkid));
	fflush(stdout);
}

struct rig_xn_clean_watch {
	char     name[32];
	uint32_t base_rx;
	int      have;
};

/*
 * WATCHER role, BEFORE half: scan the PEER's $R-namespace (read-only
 * GET_RESMASTER, Rule 8 -- no hash computed) for a candidate THIS node
 * masters with EXACTLY ONE granted lock. This is discovery only -- it waits
 * for the peer's own HOLD to land, never for a release -- so it cannot
 * deadlock against the peer's identical sequence.
 */
static void rig_xn_clean_watch_before(int fd, const struct node_cfg *c,
				      struct rig_xn_clean_watch *w)
{
	const char *peer_tag = (c->tag[0] == 'A') ? "B" : "A";
	struct vms_resmaster_args rm;
	struct vms_cluster_diag_dlm_args a;
	unsigned t, i;
	int ok = 0;

	memset(w, 0, sizeof(*w));
	for (t = 0; t < RIG_XN_GRANT_WAIT && !ok; t++) {
		for (i = 0; i < RIG_XN_CANDIDATES; i++) {
			snprintf(w->name, sizeof(w->name), "OVMX%s$R%02u",
				 peer_tag, i);
			if (rig_dlm_resmaster(fd, w->name, &rm) != 0u &&
			    rm.found != 0u && rm.is_local_master != 0u &&
			    rm.n_granted == 1u) {
				ok = 1;
				break;
			}
		}
		if (!ok)
			rig_msleep(RIG_XN_POLL_MS);
	}
	if (!ok) {
		printf("RIG-%s-RESMASTER-BEFORE NONE (masters no dedicated "
		       "peer-namespace candidate with exactly one outstanding "
		       "grant)\n", c->tag);
		fflush(stdout);
		return;
	}
	printf("RIG-%s-RESMASTER-BEFORE res=%s found=%u is_local_master=%u "
	       "n_granted=%u remote_holder_csid=0x%08x\n",
	       c->tag, w->name, (unsigned)rm.found, (unsigned)rm.is_local_master,
	       (unsigned)rm.n_granted, (unsigned)rm.remote_holder_csid);
	fflush(stdout);

	memset(&a, 0, sizeof(a));
	w->base_rx = (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_DLM, &a) == 0 &&
		      a.status == SS_NORMAL) ? a.dlm.releases_received : 0u;
	w->have = 1;
}

/*
 * WATCHER role, AFTER half. Called only once THIS node has already sent its
 * own release (see rig_xn_clean_phase) -- polls THIS node's own
 * `releases_received` for a rise past the baseline, which can only mean the
 * PEER's op-0x03 for the resource `w->name` names was actually PROCESSED,
 * and only then re-samples GET_RESMASTER. `counter_rose` is printed
 * explicitly: a stale AFTER (never having risen) must never be misread as a
 * clean delta.
 */
static void rig_xn_clean_watch_after(int fd, const struct node_cfg *c,
				     const struct rig_xn_clean_watch *w)
{
	struct vms_resmaster_args rm;
	struct vms_cluster_diag_dlm_args a;
	uint32_t cur_rx = w->base_rx;
	int rose = 0;
	unsigned t;

	if (!w->have)
		return;
	for (t = 0; t < RIG_XN_CLEAN_AFTER_WAIT; t++) {
		memset(&a, 0, sizeof(a));
		if (ioctl(fd, VMS_IOCTL_CLUSTER_DIAG_DLM, &a) == 0 &&
		    a.status == SS_NORMAL) {
			cur_rx = a.dlm.releases_received;
			if (cur_rx != w->base_rx) {
				rose = 1;
				break;
			}
		}
		rig_msleep(RIG_XN_POLL_MS);
	}
	(void)rig_dlm_resmaster(fd, w->name, &rm);
	printf("RIG-%s-RESMASTER-AFTER res=%s found=%u is_local_master=%u "
	       "n_granted=%u remote_holder_csid=0x%08x releases_received=%u "
	       "base_releases_received=%u counter_rose=%d\n",
	       c->tag, w->name, (unsigned)rm.found, (unsigned)rm.is_local_master,
	       (unsigned)rm.n_granted, (unsigned)rm.remote_holder_csid,
	       (unsigned)cur_rx, (unsigned)w->base_rx, rose);
	fflush(stdout);
}

/*
 * THE DIRECT RELEASE PROOF, end to end on this node: hold my own dedicated
 * candidate (if the peer masters one), discover the peer's dedicated
 * candidate I master and sample BEFORE, release MY OWN holder (my op-0x03,
 * for the PEER's watcher to observe), then poll for the PEER's op-0x03
 * against the resource I master and sample AFTER.
 *
 * Run AFTER rig_xnode_phase has fully completed its own release (see
 * rig_poll) so this node's releases_received baseline below is never
 * polluted by that unrelated resource's release racing in late.
 */
static void rig_xn_clean_phase(int fd, const struct node_cfg *c)
{
	struct rig_xn_clean_holder h;
	struct rig_xn_clean_watch w;

	rig_msleep(1000u);   /* let the xnode phase's own release settle */
	rig_xn_clean_hold(fd, c, &h);
	rig_xn_clean_watch_before(fd, c, &w);
	rig_xn_clean_release(fd, c, &h);
	rig_xn_clean_watch_after(fd, c, &w);
}

/* ==========================================================================
 * 6d. THE VALUE-BLOCK WRITE PROOF (rd vms-727), ON ITS OWN DEDICATED RESOURCE.
 *
 * The op-0x06 demote-from-write CONVERT must NOT run on the resource the
 * xnode phase's BLKAST/DEQ scenario (`xn`, the "$X" series) is exercising:
 * demoting that SAME holder mid-flow perturbs the contender's own blocking-
 * AST wait (measured: it drove blkasts_received to 0 on both nodes in a run
 * that otherwise held). That is exactly the lesson the DIRECT release proof
 * (6c above) already learned about the contended resource, applied again --
 * a dedicated, disjoint candidate series ("OVMX<tag>$L%02u"), a single
 * holder, no contender, never touching `xn`.
 * ========================================================================== */

/* This node's own candidate name for the dedicated LVB-write series. */
static void rig_xn_lvb_name(const struct node_cfg *c, unsigned i, char *out,
			    size_t n)
{
	snprintf(out, n, "OVMX%s$L%02u", c->tag, i);
}

struct rig_xn_lvb_holder {
	char     name[32];   /* this node's own held candidate ("" if none) */
	uint32_t lkid;
	int      held;
};

/*
 * Scan this node's own dedicated $L-series (same discovery method as
 * rig_xn_clean_hold -- GET_RESMASTER readback, Rule 8) for one the PEER
 * masters, and hold it EX with NO second $ENQ: a genuine single-holder
 * resource nothing else in this rig ever touches.
 */
static void rig_xn_lvb_hold(int fd, const struct node_cfg *c,
			    struct rig_xn_lvb_holder *h)
{
	struct vms_resmaster_args rm;
	unsigned i;

	memset(h, 0, sizeof(*h));
	for (i = 0; i < RIG_XN_CANDIDATES; i++) {
		char name[32];
		uint32_t lkid = 0u, st;

		rig_xn_lvb_name(c, i, name, sizeof(name));
		st = rig_dlm_enq(fd, name, 0u, &lkid);
		if (st != SS_NORMAL || lkid == 0u)
			continue;
		rig_xn_wait_master(fd, name, &rm);
		if (!rig_xn_is_peer_mastered(&rm)) {
			(void)rig_dlm_deq(fd, lkid);
			continue;
		}
		snprintf(h->name, sizeof(h->name), "%s", name);
		h->lkid = lkid;
		h->held = 1;
		printf("RIG-%s-LVBHOLD res=%s lkid=0x%08x master_csid=0x%08x "
		       "(dedicated single holder, no contender -- the op-0x06 "
		       "subject)\n",
		       c->tag, h->name, (unsigned)h->lkid,
		       (unsigned)rm.master_csid);
		fflush(stdout);
		return;
	}
	printf("RIG-%s-LVBHOLD NONE (no dedicated candidate was mastered by "
	       "the peer)\n", c->tag);
	fflush(stdout);
}

/*
 * THE VALUE-BLOCK WRITE (rd vms-727): the demote-from-write CONVERT that must
 * emit op-0x06. The transition is EX -> CR carrying LCK$M_VALBLK, exactly
 * the demote the own-lab capture grounded as the op-0x06 trigger (vms-727
 * c6: "a real VAX EX->CR demote emitted op-0x06, target CR not NL" -- an
 * up-convert or a read-mode holder's block is never sent). The pattern is a
 * fixed 16-byte ASCII string, chosen only to be recognisable in a hexdump;
 * nothing about it is minted onto a wire field INV-6 would otherwise leave
 * silent -- the codec carries it in the ONE body range (body[36:52])
 * grounded as caller-supplied.
 */
static const char rig_lvb_pattern[] = "OVMXLVBWRITE0001"; /* 16 bytes + NUL */

static uint32_t rig_dlm_convert_valblk(int fd, uint32_t lkid, uint32_t lkmode,
					const char *pattern16)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	a.lkmode = lkmode;
	a.flags = LCK_M_CONVERT | LCK_M_VALBLK;
	memcpy(a.valblk, pattern16, LCK_VALBLK_SIZE);
	if (ioctl(fd, VMS_IOCTL_CONVERT, &a) != 0)
		return 0u;
	return a.status;
}

static void rig_xn_lvb_write(int fd, const struct node_cfg *c,
			      const struct rig_xn_lvb_holder *h)
{
	uint32_t st;

	if (!h->held)
		return;
	st = rig_dlm_convert_valblk(fd, h->lkid, LCK_K_CRMODE, rig_lvb_pattern);
	printf("RIG-%s-LVBWRITE res=%s lkid=0x%08x status=%u mode=CR "
	       "pattern=%.16s (the demote-from-write that must emit op-0x06)\n",
	       c->tag, h->name, (unsigned)h->lkid, (unsigned)st,
	       rig_lvb_pattern);
	fflush(stdout);
	rig_msleep(2000u);   /* let the op-0x06 round trip to the master */
}

/* Release the dedicated LVB resource this node holds -- tidy-up, not part
 * of the proof (the demote already put op-0x06 on the wire). */
static void rig_xn_lvb_release(int fd, const struct node_cfg *c,
			       const struct rig_xn_lvb_holder *h)
{
	if (!h->held)
		return;
	printf("RIG-%s-LVBDEQ res=%s lkid=0x%08x status=%u\n",
	       c->tag, h->name, (unsigned)h->lkid,
	       (unsigned)rig_dlm_deq(fd, h->lkid));
	fflush(stdout);
}

/* THE VALUE-BLOCK WRITE PROOF, end to end on this node, entirely on its own
 * dedicated resource: hold, write+demote (the op-0x06 emit), release. */
static void rig_xn_lvb_phase(int fd, const struct node_cfg *c)
{
	struct rig_xn_lvb_holder h;

	rig_xn_lvb_hold(fd, c, &h);
	rig_xn_lvb_write(fd, c, &h);
	rig_xn_lvb_release(fd, c, &h);
}

/* ==========================================================================
 * 6e. THE VALUE-BLOCK READ CROSSING (rd vms-727), ON ITS OWN DEDICATED
 * RESOURCE -- the symmetric mirror of 6d above. 6d proved a HOLDER's demote
 * carries a value block TO the master over op-0x06; this proves a PEER's
 * cross-node $ENQ...LCK$M_VALBLK carries the master's OWN value block BACK,
 * in the grant reply (the op-0x01 grant-with-valblk record the codec builds,
 * rd #1190).
 *
 * A dedicated, disjoint literal name ("OVMXA$RD01") -- never the "$X"/"$L"/
 * "$R" series 6b/6c/6d already exercise (the collateral-assertion collision
 * #1187 hit is exactly why this stays off every other scenario's resource).
 *
 * NEITHER NODE IS HARD-WIRED WRITER OR READER. Both nodes run this identical
 * function; which one masters the literal name is the directory hash's
 * answer, read back (Rule 8), never assumed from a node's own tag. The
 * discovery probe is NL mode -- compatible with anything already granted on
 * either side of this race, so it can never queue regardless of which node's
 * probe reaches the (still-forming) resource first.
 */
#define RIG_LVBRD_RESNAM   "OVMXA$RD01"
#define RIG_LVBRD_POLL_MS  1000u
#define RIG_LVBRD_POLL_MAX 90u     /* 90s -- the two nodes' windows are sized
				    * to reach this phase at the same wall
				    * moment (run_cluster_genesis_2node.sh),
				    * so this is slack, not the sync mechanism */

/* 16 bytes + NUL, same convention as rig_lvb_pattern above. */
static const char rig_lvbrd_pattern[] = "OVMXLVBREAD00001";

/*
 * A fresh (non-CONVERT) $ENQ carrying LCK$M_VALBLK with an ALL-ZERO block.
 * Per the engine's local-grant path (vms_lock.c enq_core_ex): a zero-valued
 * block on grant means READ the resource's value block into this lock,
 * rather than write one -- this is the genuine $ENQ...LCK$M_VALBLK "peek"
 * both the discovery probe and the reader's real read use.
 */
static uint32_t rig_dlm_enq_peek_valblk(int fd, const char *resnam,
					uint32_t lkmode, uint32_t *lkid_out,
					uint8_t *valblk_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = lkmode;
	a.flags = LCK_M_VALBLK;
	snprintf(a.resnam, sizeof(a.resnam), "%s", resnam);
	if (ioctl(fd, VMS_IOCTL_ENQ, &a) != 0)
		return 0u;
	*lkid_out = a.lkid;
	if (valblk_out)
		memcpy(valblk_out, a.valblk, LCK_VALBLK_SIZE);
	return a.status;
}

/*
 * A 16-byte value block rendered as ASCII, unprintable bytes shown as '.' --
 * the same convention scan_dlm_wire.py's op06_detail/op01 detail use
 * (0x21..0x7e, deliberately EXCLUDING the literal space 0x20: it would split
 * this token when the host script's own `tr ' ' '\n'` scraper tokenises the
 * printed line), so a hexdump is never the only way to read what crossed.
 */
static void rig_lvbrd_ascii(const uint8_t *v, char *out, size_t n)
{
	size_t i;

	for (i = 0; i < LCK_VALBLK_SIZE && i + 1 < n; i++)
		out[i] = (v[i] >= 0x21 && v[i] < 0x7f) ? (char)v[i] : '.';
	out[i] = '\0';
}

static int rig_lvbrd_is_pattern(const uint8_t *v)
{
	return memcmp(v, rig_lvbrd_pattern, LCK_VALBLK_SIZE) == 0;
}

/*
 * Poll $GETLKI until a cross-node request has actually RESOLVED, not merely
 * been accepted into the proxy LKB (fc8540ae-adjacent lesson, applied here):
 * the ENQ ioctl for a resource this node does not master returns ASYNC, with
 * the grant reply still in flight over the wire (rig_dlm_enq's own doc
 * comment says as much). `lock->granted_mode` reads 0 until that reply is
 * processed -- and since the mode requested here is CR (1), never NL (0),
 * "still 0" and "genuinely granted" cannot be confused the way they can for
 * an NL request (see the XN-CONTEND-MODE comment elsewhere in this file for
 * that ambiguity). Returns 1 once resolved, 0 on timeout -- never guesses.
 */
#define RIG_LVBRD_GRANT_WAIT 12u   /* x RIG_XN_POLL_MS: 3s cap PER ATTEMPT --
				    * a CR request is always compatible with
				    * the writer's held CR, so real resolution
				    * is one round trip; bounded here only so
				    * a genuine failure cannot blow the outer
				    * RIG_LVBRD_POLL_MAX loop's own budget */

static int rig_lvbrd_wait_granted(int fd, uint32_t lkid)
{
	unsigned t;

	for (t = 0; t < RIG_LVBRD_GRANT_WAIT; t++) {
		struct vms_getlki_args g;

		memset(&g, 0, sizeof(g));
		g.lkid = lkid;
		if (ioctl(fd, VMS_IOCTL_GETLKI, &g) == 0 &&
		    g.status == SS_NORMAL && g.granted_mode == LCK_K_CRMODE)
			return 1;
		rig_msleep(RIG_XN_POLL_MS);
	}
	return 0;
}

/*
 * THE READER'S HALF: this node does NOT master the name. Poll -- a fresh
 * $ENQ...LCK$M_VALBLK (a genuine NEW cross-node request each attempt, never
 * the same stale lkid: a lock's own cached value block is only ever the
 * snapshot taken at ITS OWN grant, vms_lock.c never re-pushes a later
 * writer's update to an already-granted holder), mode CR so it is always
 * compatible with the writer's held CR (rig_lvbrd_write_and_hold) and never
 * queues -- WAIT for that grant to genuinely resolve, THEN $GETLKI, exactly
 * as the task's own read step is worded -- until the master's write is seen,
 * or the deadline passes. A miss is printed as a miss (INV-6): fabricating a
 * match this run never measured is the placeholder that crashed a real VAX.
 */
static void rig_lvbrd_read_phase(int fd, const struct node_cfg *c)
{
	struct vms_getlki_args g;
	uint8_t valblk[LCK_VALBLK_SIZE];
	char ascii[LCK_VALBLK_SIZE + 1];
	uint32_t lkid = 0u, enq_st = 0u, getlki_st = 0u;
	unsigned attempt;
	int matched = 0;

	memset(valblk, 0, sizeof(valblk));
	for (attempt = 0; attempt < RIG_LVBRD_POLL_MAX; attempt++) {
		uint8_t zero[LCK_VALBLK_SIZE];

		memset(zero, 0, sizeof(zero));
		enq_st = rig_dlm_enq_peek_valblk(fd, RIG_LVBRD_RESNAM,
						 LCK_K_CRMODE, &lkid, zero);
		if (enq_st == SS_NORMAL && lkid != 0u) {
			(void)rig_lvbrd_wait_granted(fd, lkid);
			memset(&g, 0, sizeof(g));
			g.lkid = lkid;
			if (ioctl(fd, VMS_IOCTL_GETLKI, &g) == 0) {
				getlki_st = g.status;
				memcpy(valblk, g.valblk, sizeof(valblk));
			}
			(void)rig_dlm_deq(fd, lkid);
			matched = rig_lvbrd_is_pattern(valblk);
			if (matched)
				break;
		}
		rig_msleep(RIG_LVBRD_POLL_MS);
	}

	rig_lvbrd_ascii(valblk, ascii, sizeof(ascii));
	printf("RIG-%s-GETLKI res=%s valblk_ascii=%s matched=%d attempts=%u "
	       "enq_status=%u getlki_status=%u (cross-node $ENQ...LCK$M_VALBLK "
	       "read of the peer master's value block, rd vms-727)\n",
	       c->tag, RIG_LVBRD_RESNAM, ascii, matched, attempt + 1u,
	       (unsigned)enq_st, (unsigned)getlki_st);
	fflush(stdout);
}

/*
 * THE WRITER'S HALF: this node masters the name. Convert the discovery
 * probe's own NL grant up to CR carrying the pattern (the same
 * demote-with-valblk mechanic 6d's rig_dlm_convert_valblk already proved,
 * reused verbatim) and HOLD -- no DEQ. This is entirely local (this node IS
 * the master, so vms_lock.c applies the value straight to res->valblk; no
 * wire op is expected or needed for this half), and the resource must
 * survive, written, for the rest of this node's run so the peer's read can
 * find it.
 */
static void rig_lvbrd_write_and_hold(int fd, const struct node_cfg *c,
				     uint32_t probe_lkid)
{
	uint32_t st;

	st = rig_dlm_convert_valblk(fd, probe_lkid, LCK_K_CRMODE,
				    rig_lvbrd_pattern);
	printf("RIG-%s-LVBRDHOLD res=%s lkid=0x%08x status=%u mode=CR "
	       "pattern=%.16s (this node IS the master for the READ crossing "
	       "-- writes+holds the value block for the peer's cross-node "
	       "read, rd vms-727)\n",
	       c->tag, RIG_LVBRD_RESNAM, (unsigned)probe_lkid, (unsigned)st,
	       rig_lvbrd_pattern);
	fflush(stdout);
}

static void rig_lvbrd_phase(int fd, const struct node_cfg *c)
{
	struct vms_resmaster_args rm;
	uint32_t probe_lkid = 0u, st;

	st = rig_dlm_enq_peek_valblk(fd, RIG_LVBRD_RESNAM, LCK_K_NLMODE,
				     &probe_lkid, NULL);
	if (st != SS_NORMAL || probe_lkid == 0u) {
		printf("RIG-%s-LVBRD-PROBE res=%s status=%u (not enqueued)\n",
		       c->tag, RIG_LVBRD_RESNAM, (unsigned)st);
		fflush(stdout);
		return;
	}
	rig_xn_wait_master(fd, RIG_LVBRD_RESNAM, &rm);

	if (rm.found && rm.is_local_master) {
		rig_lvbrd_write_and_hold(fd, c, probe_lkid);
		/* deliberately no DEQ: res->valblk must survive, written, for
		 * the peer's cross-node read for the rest of this node's run */
	} else if (rig_xn_is_peer_mastered(&rm)) {
		printf("RIG-%s-LVBRD-PEER res=%s master_csid=0x%08x (the peer "
		       "masters this name -- this node is the READER)\n",
		       c->tag, RIG_LVBRD_RESNAM, (unsigned)rm.master_csid);
		fflush(stdout);
		(void)rig_dlm_deq(fd, probe_lkid);
		rig_lvbrd_read_phase(fd, c);
	} else {
		printf("RIG-%s-LVBRD NONE (the executive could not resolve a "
		       "master for %s)\n", c->tag, RIG_LVBRD_RESNAM);
		fflush(stdout);
		(void)rig_dlm_deq(fd, probe_lkid);
	}
}

/*
 * THE SURVIVAL LINE. Printed AFTER the phase, and after a LINGER long enough
 * for the PEER's frames to have arrived here -- because the property being
 * measured is not "this node emitted", it is "the node that RECEIVED an
 * op-0x03 and an op-0x04 it has no receive half for is still a sane member".
 * The counters are re-read here rather than remembered, so the line reports
 * what the executive holds at that moment (INV-6).
 */
static void rig_xn_survival(int fd, const struct node_cfg *c, unsigned linger_s)
{
	unsigned t;

	for (t = 0; t < linger_s; t++)
		rig_msleep(1000u);
	rig_dump_dlm(fd, c, "survival");
}

/*
 * THE RUN, in the order the proof needs.
 *
 * The verdict line is printed TWICE and that is deliberate. The first is the
 * GENESIS verdict: membership as it stood when the discovery window closed,
 * which is what the original rig measured and is unchanged. The second comes
 * after the cross-node phase and after a linger long enough for the PEER's
 * op-0x03 and op-0x04 to have arrived here -- so it says something the first
 * cannot: that a node which RECEIVED two frames its arm has no receive half
 * for is still a member, still counting two nodes, and still has its two
 * membership projections agreeing.
 *
 * The host verdict reads the LAST such line, so in a run with a cross-node
 * phase it is reading the post-frame one -- which is exactly the assertion the
 * never-crash-a-peer proof needs -- and in a run without one the two lines are
 * the same reading taken twice.
 */
static int rig_poll(int fd, const struct node_cfg *c)
{
	struct rig_sample s;
	struct rig_xnode xn;
	unsigned t;

	for (t = 0; t < c->window; t++) {
		sleep(1);
		rig_sample_take(fd, &s);
		rig_report(c, t + 1u, &s);
	}
	rig_sample_take(fd, &s);
	rig_verdict(c, &s);
	rig_dlm_probe(fd, c);

	if (c->xnode) {
		rig_xnode_phase(fd, c, &xn);
		rig_xn_clean_phase(fd, c);
		rig_xn_lvb_phase(fd, c);
		rig_lvbrd_phase(fd, c);
		rig_xn_survival(fd, c, c->linger);
		rig_sample_take(fd, &s);
		rig_verdict(c, &s);   /* the SURVIVAL reading -- see above */
	}

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
