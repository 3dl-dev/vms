/*
 * scs_config.c - SB / PB / PDT queue mechanics (vms-7be).
 *
 * See scs_config.h for the provenance block. Every rule implemented here is
 * cited to a page of Roy G. Davis, *VAXcluster Principles*, Digital Press 1993,
 * ch. 2. This file touches no frame and no socket: it is pure state.
 */
#include "scs_config.h"

#include <stdlib.h>
#include <string.h>

/* --- pool helpers --------------------------------------------------------- */

static struct scs_sb *sb_alloc(struct scs_config *cfg)
{
    for (unsigned i = 0; i < SCS_CONFIG_MAX_SB; i++) {
        if (!cfg->sb_pool[i].in_use) {
            struct scs_sb *sb = &cfg->sb_pool[i];
            memset(sb, 0, sizeof(*sb));
            sb->in_use = 1;
            return sb;
        }
    }
    return NULL;
}

static void sb_free(struct scs_sb *sb)
{
    memset(sb, 0, sizeof(*sb));
}

static struct scs_pb *pb_alloc(struct scs_config *cfg)
{
    for (unsigned i = 0; i < SCS_CONFIG_MAX_PB; i++) {
        if (!cfg->pb_pool[i].in_use) {
            struct scs_pb *pb = &cfg->pb_pool[i];
            memset(pb, 0, sizeof(*pb));
            pb->in_use = 1;
            return pb;
        }
    }
    return NULL;
}

/* --- queue helpers -------------------------------------------------------- */

/* Insert an SB at the head of the configuration queue (p. 2-17/2-18). */
static void cfg_queue_insert(struct scs_config *cfg, struct scs_sb *sb)
{
    sb->prev = NULL;
    sb->next = cfg->sb_head;
    if (cfg->sb_head != NULL) {
        cfg->sb_head->prev = sb;
    }
    cfg->sb_head = sb;
}

/* Queue a PB to the head of a PDT's formative queue (p. 2-20/2-21). */
static void pdt_queue_insert(struct scs_pdt *pdt, struct scs_pb *pb)
{
    pb->prev = NULL;
    pb->next = pdt->formative_head;
    if (pdt->formative_head != NULL) {
        pdt->formative_head->prev = pb;
    }
    pdt->formative_head = pb;
    pb->on_pdt = 1;
}

static void pdt_queue_remove(struct scs_pdt *pdt, struct scs_pb *pb)
{
    if (pb->prev != NULL) {
        pb->prev->next = pb->next;
    } else if (pdt->formative_head == pb) {
        pdt->formative_head = pb->next;
    }
    if (pb->next != NULL) {
        pb->next->prev = pb->prev;
    }
    pb->next = NULL;
    pb->prev = NULL;
    pb->on_pdt = 0;
}

/* Queue a PB to an SB's open-circuit queue (p. 2-17). */
static void sb_queue_insert(struct scs_sb *sb, struct scs_pb *pb)
{
    pb->prev = NULL;
    pb->next = sb->pb_head;
    if (sb->pb_head != NULL) {
        sb->pb_head->prev = pb;
    }
    sb->pb_head = pb;
    pb->on_pdt = 0;
}

static void sb_queue_remove(struct scs_sb *sb, struct scs_pb *pb)
{
    if (pb->prev != NULL) {
        pb->prev->next = pb->next;
    } else if (sb->pb_head == pb) {
        sb->pb_head = pb->next;
    }
    if (pb->next != NULL) {
        pb->next->prev = pb->prev;
    }
    pb->next = NULL;
    pb->prev = NULL;
}

static int system_id_is_set(const uint8_t id[SCS_SYSTEM_ID_LEN])
{
    for (unsigned i = 0; i < SCS_SYSTEM_ID_LEN; i++) {
        if (id[i] != 0) {
            return 1;
        }
    }
    return 0;
}

/* --- lifecycle ------------------------------------------------------------ */

void scs_config_init(struct scs_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

void scs_pdt_init(struct scs_pdt *pdt, enum scs_port_type port_type,
                  uint32_t max_xfer_bytes)
{
    if (pdt == NULL) {
        return;
    }
    memset(pdt, 0, sizeof(*pdt));
    pdt->port_type = port_type;
    pdt->max_xfer_bytes = max_xfer_bytes;
}

void scs_sb_apply_info(struct scs_sb *sb, const struct scs_sb_info *info)
{
    if (sb == NULL || info == NULL) {
        return;
    }
    sb->cpu_type = info->cpu_type;
    sb->hw_rev = info->hw_rev;
    sb->os_version = info->os_version;
    sb->incarnation = info->incarnation;
    if (system_id_is_set(info->system_id)) {
        memcpy(sb->system_id, info->system_id, SCS_SYSTEM_ID_LEN);
    }
    if (info->os_name != NULL) {
        strncpy(sb->os_name, info->os_name, SCS_SB_OSNAME_LEN);
        sb->os_name[SCS_SB_OSNAME_LEN] = '\0';
    }
    if (info->node_name != NULL) {
        strncpy(sb->node_name, info->node_name, SCS_SB_NODENAME_LEN);
        sb->node_name[SCS_SB_NODENAME_LEN] = '\0';
    }
}

/* --- path blocks ---------------------------------------------------------- */

struct scs_pb *scs_pb_create(struct scs_config *cfg, struct scs_pdt *pdt,
                             const uint8_t remote_port_addr[SCS_PORT_ADDR_LEN],
                             enum scs_port_type remote_port_type)
{
    if (cfg == NULL || pdt == NULL || remote_port_addr == NULL) {
        return NULL;
    }
    struct scs_pb *pb = pb_alloc(cfg);
    if (pb == NULL) {
        return NULL;
    }
    /* "When the PB is first initialized, it is marked to indicate that the
     * state of the virtual circuit is CLOSED." (p. 2-11) */
    pb->vc_state = SCS_VC_CLOSED;
    pb->remote_port_type = remote_port_type;
    pb->remote_port_state = SCS_PORT_STATE_UNKNOWN;
    memcpy(pb->remote_port_addr, remote_port_addr, SCS_PORT_ADDR_LEN);
    pb->pdt = pdt;
    pb->sb = NULL;
    /* "When a Path Block is created, it is queued to the PDT corresponding to
     * the local port involved in forming the new virtual circuit." (p. 2-20) */
    pdt_queue_insert(pdt, pb);
    return pb;
}

void scs_pb_set_vc_state(struct scs_pb *pb, enum scs_vc_state state)
{
    if (pb == NULL) {
        return;
    }
    pb->vc_state = state;
}

struct scs_sb *scs_pb_attach_formative_sb(struct scs_config *cfg, struct scs_pb *pb,
                                          const struct scs_sb_info *info)
{
    if (cfg == NULL || pb == NULL || info == NULL) {
        return NULL;
    }
    if (pb->sb != NULL) {
        /* A retransmitted START/STACK re-describes the same node; and an
         * already-open PB points at a queued (non-formative) SB, which the
         * formation path must not overwrite wholesale. */
        if (pb->sb->formative) {
            scs_sb_apply_info(pb->sb, info);
        }
        return pb->sb;
    }
    struct scs_sb *sb = sb_alloc(cfg);
    if (sb == NULL) {
        return NULL;
    }
    sb->formative = 1;
    scs_sb_apply_info(sb, info);
    /* "The address of this System Block is placed in the Path Block; and, of
     * course, this System Block is called a formative System Block." (p. 2-20) */
    pb->sb = sb;
    return sb;
}

struct scs_sb *scs_pb_learn_system_addr(struct scs_config *cfg, struct scs_pb *pb,
                                        const uint8_t system_id[SCS_SYSTEM_ID_LEN])
{
    if (cfg == NULL || pb == NULL || system_id == NULL) {
        return NULL;
    }
    if (pb->sb == NULL) {
        struct scs_sb *sb = sb_alloc(cfg);
        if (sb == NULL) {
            return NULL;
        }
        sb->formative = 1;
        memcpy(sb->system_id, system_id, SCS_SYSTEM_ID_LEN);
        pb->sb = sb;
        return sb;
    }
    /* Unconditional: this records what the wire said, exactly as the per-peer
     * field it replaces did -- including a peer that stops advertising an
     * address. Deciding that a re-learned address is untrustworthy is the job of
     * the identity checks in the p. 2-21 footnote, which are a separate item and
     * deliberately NOT smuggled in here. */
    memcpy(pb->sb->system_id, system_id, SCS_SYSTEM_ID_LEN);
    return pb->sb;
}

/* --- the p. 2-21 footnote masquerade tests (vms-22e) ---------------------- */

int scs_masquerade_tests_enabled(void)
{
    /* Read fresh every call (see scs_config.h): a cached answer could not be
     * bracketed by a test, and guardrail 23 requires the switch be RUN. */
    const char *v = getenv("OVMX_NO_MASQUERADE_TESTS");
    return (v != NULL && v[0] == '1' && v[1] == '\0') ? 0 : 1;
}

const char *scs_masquerade_result_name(enum scs_masquerade_result r)
{
    switch (r) {
    case SCS_MASQ_PASS:
        return "PASS";
    case SCS_MASQ_FAIL_NODE_NAME:
        return "SCS Node Names differ for a matching SCS System ID";
    case SCS_MASQ_FAIL_SYSTEM_ID:
        return "SCS System IDs differ for a matching SCS Node Name";
    case SCS_MASQ_FAIL_INCARNATION:
        return "incarnation numbers differ with a Path Block still queued";
    default:
        return "?";
    }
}

enum scs_masquerade_result scs_config_masquerade_check(const struct scs_config *cfg,
                                                       const struct scs_sb *formative)
{
    if (cfg == NULL || formative == NULL) {
        return SCS_MASQ_PASS;
    }
    if (!scs_masquerade_tests_enabled()) {
        return SCS_MASQ_PASS;
    }

    for (const struct scs_sb *old = cfg->sb_head; old != NULL; old = old->next) {
        if (old == formative) {
            continue;
        }

        /* An input the local system has never learned cannot convict a node:
         * "known" gates each comparison, "match" is the comparison itself. */
        int id_known = system_id_is_set(formative->system_id) &&
                       system_id_is_set(old->system_id);
        int id_match = id_known &&
                       memcmp(old->system_id, formative->system_id,
                              SCS_SYSTEM_ID_LEN) == 0;
        int name_known = formative->node_name[0] != '\0' && old->node_name[0] != '\0';
        int name_match = name_known &&
                         strcmp(old->node_name, formative->node_name) == 0;

        /* "if the SCS System ID in the formative System Block matches the SCS
         * System ID in a System Block already in the Configuration Queue, the
         * SCS Node Names must also match" (p. 2-21 footnote) */
        if (id_match && name_known && !name_match) {
            return SCS_MASQ_FAIL_NODE_NAME;
        }

        /* "The converse is also true." (same footnote) */
        if (name_match && id_known && !id_match) {
            return SCS_MASQ_FAIL_SYSTEM_ID;
        }

        /* "If both items match, and if there is a Path Block already queued to
         * the System Block in the Configuration Queue, then the 64-bit
         * incarnation numbers must also match." (same footnote)
         *
         * The p. 2-21 Note is the other side of this: with NO Path Block queued
         * the old SB is REFRESHED instead ("the remote node was once in the
         * cluster, departed, and is now rebooting"), so a rebooted node's new
         * incarnation is expected there and must not be treated as a masquerade.
         * Hence the old->pb_head guard, which is the rule, not an escape hatch. */
        if (id_match && name_match && old->pb_head != NULL) {
            if (formative->incarnation != 0 && old->incarnation != 0 &&
                formative->incarnation != old->incarnation) {
                return SCS_MASQ_FAIL_INCARNATION;
            }
        }
    }
    return SCS_MASQ_PASS;
}

enum scs_open_result scs_pb_open(struct scs_config *cfg, struct scs_pb *pb)
{
    if (cfg == NULL || pb == NULL || pb->sb == NULL) {
        return SCS_OPEN_ERROR;
    }
    if (!pb->on_pdt) {
        /* Already open and queued to an SB -- nothing to move (an "implied ACK"
         * on an already-OPEN circuit, p. 2-16). */
        pb->vc_state = SCS_VC_OPEN;
        return SCS_OPEN_EXISTING_SB;
    }

    struct scs_sb *formative = pb->sb;

    /* vms-22e: "At this time, special tests are made to ensure that the remote
     * node is not masquerading as a node already known to the local system ...
     * Virtual circuit formation is abandoned if any of these tests fail."
     * (p. 2-21 footnote). BEFORE the queue moves: an abandoned circuit must
     * leave the configuration queue exactly as it found it. */
    enum scs_masquerade_result masq = scs_config_masquerade_check(cfg, formative);
    if (masq != SCS_MASQ_PASS) {
        pb->masquerade_fail = (int)masq;
        /* p. 2-14 / scs_vc.h: an abandoned circuit is simply not formed, so its
         * state IS CLOSED and fsm.abandoned records why. The PB stays formative
         * on its PDT; tearing it down is the port driver's job (vms-17f). */
        pb->vc_state = SCS_VC_CLOSED;
        pb->fsm.abandoned = 1;
        return SCS_OPEN_ABANDONED_MASQUERADE;
    }
    pb->masquerade_fail = (int)SCS_MASQ_PASS;

    /* vms-348: scs_config_find_sb() only ever searches cfg->sb_head, and a
     * formative SB is never linked into cfg->sb_head until the branch below
     * inserts it (cfg_queue_insert) -- so `old == formative` cannot happen
     * here; the search space structurally excludes `formative` by construction. */
    struct scs_sb *old = scs_config_find_sb(cfg, formative->system_id);

    pdt_queue_remove(pb->pdt, pb);
    pb->vc_state = SCS_VC_OPEN;

    if (old == NULL) {
        /* "If there is no other System Block corresponding to the remote node in
         * the VMS SCA Configuration Queue ... the Path Block is dequeued from the
         * PDT, the System Block is inserted into the Configuration Queue, and the
         * Path Block is queued to the System Block. The System Block and Path
         * Block are no longer considered 'formative'." (p. 2-21) */
        formative->formative = 0;
        cfg_queue_insert(cfg, formative);
        sb_queue_insert(formative, pb);
        return SCS_OPEN_NEW_SB;
    }

    /* "Suppose that a System Block corresponding to the remote node is already
     * present in the Configuration Queue. The formative Path Block is then queued
     * to the old System Block, and the formative System Block is discarded."
     * (p. 2-21) */
    enum scs_open_result result = SCS_OPEN_EXISTING_SB;
    if (old->pb_head == NULL) {
        /* THE NOTE (p. 2-21): "If there are no other Path Blocks queued to the
         * old System Block, the new Path Block represents the only open virtual
         * circuit with the remote node. If this is the case, the old System Block
         * is refreshed based on the contents of the formative System Block before
         * the formative System Block is discarded. Typically, this happens when
         * the remote node was once in the cluster, departed, and is now
         * rebooting." */
        struct scs_sb_info refresh;
        memset(&refresh, 0, sizeof(refresh));
        refresh.cpu_type = formative->cpu_type;
        refresh.hw_rev = formative->hw_rev;
        refresh.os_name = formative->os_name;
        refresh.os_version = formative->os_version;
        memcpy(refresh.system_id, formative->system_id, SCS_SYSTEM_ID_LEN);
        refresh.node_name = formative->node_name;
        refresh.incarnation = formative->incarnation;
        scs_sb_apply_info(old, &refresh);
        result = SCS_OPEN_EXISTING_REFRESHED;
    }
    pb->sb = old;
    sb_queue_insert(old, pb);
    sb_free(formative);
    return result;
}

enum scs_pb_close_result scs_pb_close(struct scs_config *cfg, struct scs_pb *pb)
{
    if (cfg == NULL || pb == NULL || !pb->in_use) {
        return SCS_PB_CLOSE_NOTHING;
    }
    /* p. 2-28: "If the circuit is broken for any reason, it is then a relatively
     * simple matter to scan this queue to determine which connections have also
     * been lost, and to notify the interested SYSAPs." That is the RATIONALE the
     * book gives for queueing CDTs to the Path Block -- it is not an ordering
     * mandate. Requiring the scan to happen BEFORE the circuit structure is
     * destroyed is an OVMX design choice (vms-228): this module cannot perform
     * the scan itself (the CDT layer sits above this one), so instead of relying
     * on callers to get the order right, a PB with connections still queued to
     * it is refused rather than closed. */
    if (pb->cdt_head != NULL) {
        return SCS_PB_CLOSE_CONNECTIONS_QUEUED;
    }
    if (pb->on_pdt) {
        pdt_queue_remove(pb->pdt, pb);
        /* A formative SB has no other owner -- it dies with its PB. */
        if (pb->sb != NULL && pb->sb->formative) {
            sb_free(pb->sb);
        }
    } else if (pb->sb != NULL) {
        sb_queue_remove(pb->sb, pb);
        /* The SB STAYS in the configuration queue: VMS keeps SBs for nodes with
         * which it has had at least one open virtual circuit (p. 2-17). */
    }
    memset(pb, 0, sizeof(*pb));
    return SCS_PB_CLOSE_OK;
}

/* --- lookups -------------------------------------------------------------- */

struct scs_pb *scs_config_find_pb(struct scs_config *cfg, const struct scs_pdt *pdt,
                                  const uint8_t remote_port_addr[SCS_PORT_ADDR_LEN])
{
    if (cfg == NULL || remote_port_addr == NULL) {
        return NULL;
    }
    /* The pools are small and flat; scanning them finds a PB whether it is on a
     * PDT's formative queue or on an SB's open queue. */
    for (unsigned i = 0; i < SCS_CONFIG_MAX_PB; i++) {
        struct scs_pb *pb = &cfg->pb_pool[i];
        if (!pb->in_use) {
            continue;
        }
        if (pdt != NULL && pb->pdt != pdt) {
            continue;
        }
        if (memcmp(pb->remote_port_addr, remote_port_addr, SCS_PORT_ADDR_LEN) == 0) {
            return pb;
        }
    }
    return NULL;
}

struct scs_sb *scs_config_find_sb(struct scs_config *cfg,
                                  const uint8_t system_id[SCS_SYSTEM_ID_LEN])
{
    if (cfg == NULL || system_id == NULL || !system_id_is_set(system_id)) {
        return NULL;
    }
    for (struct scs_sb *sb = cfg->sb_head; sb != NULL; sb = sb->next) {
        if (memcmp(sb->system_id, system_id, SCS_SYSTEM_ID_LEN) == 0) {
            return sb;
        }
    }
    return NULL;
}

struct scs_sb *scs_config_insert_sb(struct scs_config *cfg, const struct scs_sb_info *info)
{
    if (cfg == NULL || info == NULL) {
        return NULL;
    }
    struct scs_sb *existing = scs_config_find_sb(cfg, info->system_id);
    if (existing != NULL) {
        return existing;
    }
    struct scs_sb *sb = sb_alloc(cfg);
    if (sb == NULL) {
        return NULL;
    }
    sb->formative = 0;
    scs_sb_apply_info(sb, info);
    cfg_queue_insert(cfg, sb);
    return sb;
}

unsigned scs_config_sb_count(const struct scs_config *cfg)
{
    unsigned n = 0;
    if (cfg == NULL) {
        return 0;
    }
    for (const struct scs_sb *sb = cfg->sb_head; sb != NULL; sb = sb->next) {
        n++;
    }
    return n;
}

unsigned scs_sb_pb_count(const struct scs_sb *sb)
{
    unsigned n = 0;
    if (sb == NULL) {
        return 0;
    }
    for (const struct scs_pb *pb = sb->pb_head; pb != NULL; pb = pb->next) {
        n++;
    }
    return n;
}

unsigned scs_pdt_formative_count(const struct scs_pdt *pdt)
{
    unsigned n = 0;
    if (pdt == NULL) {
        return 0;
    }
    for (const struct scs_pb *pb = pdt->formative_head; pb != NULL; pb = pb->next) {
        n++;
    }
    return n;
}

const char *scs_vc_state_name(enum scs_vc_state state)
{
    switch (state) {
    case SCS_VC_CLOSED:
        return "CLOSED";
    case SCS_VC_START_SENT:
        return "START SENT";
    case SCS_VC_START_RECEIVED:
        return "START RECEIVED";
    case SCS_VC_OPEN:
        return "OPEN";
    default:
        return "?";
    }
}

/* --- CONFIG_SYS / CONFIG_PATH (vms-398, p. 2-47) -------------------------- */

int scs_config_sys(struct scs_config *cfg, const uint8_t system_id[SCS_SYSTEM_ID_LEN],
                   struct scs_config_sys_info *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (cfg == NULL || system_id == NULL || out == NULL) {
        return 0;
    }
    /* CONFIG_SYS queries the System List (p. 2-17), i.e. the configuration
     * queue -- not in-formation state. scs_config_find_sb already restricts
     * itself to cfg->sb_head, which never holds a formative SB (scs_pb_open
     * moves an SB there only as it stops being formative). */
    struct scs_sb *sb = scs_config_find_sb(cfg, system_id);
    if (sb == NULL) {
        return 0;
    }

    memcpy(out->system_id, sb->system_id, SCS_SYSTEM_ID_LEN);
    out->first_pb = sb->pb_head;

    /* max_datagram_size / max_message_size: no such SB field exists (see the
     * header note) -- left at 0, HAVE bits left unset. */

    if (sb->os_name[0] != '\0') {
        memcpy(out->software_type, sb->os_name, sizeof(out->software_type));
        out->have |= SCS_CONFIG_SYS_HAVE_SOFTWARE_TYPE;
    }
    if (sb->os_version != 0) {
        out->software_version = sb->os_version;
        out->have |= SCS_CONFIG_SYS_HAVE_SOFTWARE_VERSION;
    }
    if (sb->node_name[0] != '\0') {
        memcpy(out->node_name, sb->node_name, sizeof(out->node_name));
        out->have |= SCS_CONFIG_SYS_HAVE_NODE_NAME;
    }

    return 1;
}

int scs_config_path(const struct scs_pb *pb, struct scs_config_path_info *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (pb == NULL || out == NULL || !pb->in_use) {
        return 0;
    }

    out->vc_state = pb->vc_state;
    out->remote_port_type = pb->remote_port_type;
    out->remote_port_state = pb->remote_port_state;
    memcpy(out->remote_port_addr, pb->remote_port_addr, SCS_PORT_ADDR_LEN);
    out->sb = pb->sb;
    out->next = pb->next;
    out->on_formative_queue = pb->on_pdt;

    return 1;
}

struct scs_pb *scs_config_select_vc(struct scs_config *cfg,
                                    const uint8_t system_id[SCS_SYSTEM_ID_LEN])
{
    struct scs_config_sys_info sys_info;
    if (!scs_config_sys(cfg, system_id, &sys_info)) {
        return NULL;
    }
    /* "examines each Path Block in turn until it finds one whose virtual
     * circuit is OPEN" (p. 2-47). The examination goes through CONFIG_PATH --
     * that is the query the book defines for reading a Path Block's virtual-
     * circuit state and its link to the next PB, and using it here means the
     * scan sees exactly what any other CONFIG_PATH caller would see. See the
     * header note: OVMX has only one interconnect type in production, so there
     * is no CI-vs-Ethernet preference to apply -- first OPEN PB found wins. */
    struct scs_config_path_info path_info;
    for (struct scs_pb *pb = sys_info.first_pb; pb != NULL; pb = path_info.next) {
        if (!scs_config_path(pb, &path_info)) {
            break; /* not a live PB: the queue is corrupt, stop rather than guess */
        }
        if (path_info.vc_state == SCS_VC_OPEN) {
            return pb;
        }
    }
    return NULL;
}
