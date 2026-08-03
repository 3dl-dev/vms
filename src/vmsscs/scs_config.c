/*
 * scs_config.c - SB / PB / PDT queue mechanics (vms-7be).
 *
 * See scs_config.h for the provenance block. Every rule implemented here is
 * cited to a page of Roy G. Davis, *VAXcluster Principles*, Digital Press 1993,
 * ch. 2. This file touches no frame and no socket: it is pure state.
 */
#include "scs_config.h"

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
    struct scs_sb *old = scs_config_find_sb(cfg, formative->system_id);

    pdt_queue_remove(pb->pdt, pb);
    pb->vc_state = SCS_VC_OPEN;

    if (old == NULL || old == formative) {
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

void scs_pb_close(struct scs_config *cfg, struct scs_pb *pb)
{
    if (cfg == NULL || pb == NULL || !pb->in_use) {
        return;
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
