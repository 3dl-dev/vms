/*
 * dnet_adjacency.c - DECnet Phase IV routing-layer adjacency state machine
 *                    (rd vms-b15). See dnet_adjacency.h for the full clean-room
 *                    provenance statement, the state diagram, and exactly which
 *                    timer value is oracle-captured (T3 = 15 s, vms-3be) versus
 *                    spec-derived (BCT3MULT, DNA Phase IV, not on the wire).
 *
 * Pure, deterministic SM + timer logic over the rung-1 HELLO codec: no socket,
 * no thread, no allocation, no wall clock. The caller injects a monotonic tick
 * into every entry point.
 */
#include "dnet_adjacency.h"

#include <string.h>

static int id_eq(const uint8_t a[DNET_ADDR_LEN], const uint8_t b[DNET_ADDR_LEN])
{
    return memcmp(a, b, DNET_ADDR_LEN) == 0;
}

int dnet_adj_init(struct dnet_adjacency *sm, const uint8_t my_id[DNET_ADDR_LEN],
                  uint16_t t3, uint16_t bct3mult, dnet_tick_t now)
{
    if (!sm || !my_id)
        return DNET_ADJ_EINVAL;

    memset(sm, 0, sizeof(*sm));
    memcpy(sm->my_id, my_id, DNET_ADDR_LEN);
    sm->t3       = t3 ? t3 : (uint16_t)DNET_T3_DEFAULT;
    sm->bct3mult = bct3mult ? bct3mult : (uint16_t)DNET_BCT3MULT_DEFAULT;
    sm->count    = 0;
    sm->next_hello_due = now + sm->t3;
    return DNET_ADJ_OK;
}

struct dnet_adj_neighbor *dnet_adj_find(struct dnet_adjacency *sm,
                                        const uint8_t id[DNET_ADDR_LEN])
{
    if (!sm || !id)
        return NULL;
    for (size_t i = 0; i < DNET_ADJ_MAX_NEIGHBORS; i++) {
        if (sm->nbr[i].in_use && id_eq(sm->nbr[i].id, id))
            return &sm->nbr[i];
    }
    return NULL;
}

/* Find a free slot, or NULL if the table is full. */
static struct dnet_adj_neighbor *alloc_slot(struct dnet_adjacency *sm)
{
    for (size_t i = 0; i < DNET_ADJ_MAX_NEIGHBORS; i++) {
        if (!sm->nbr[i].in_use)
            return &sm->nbr[i];
    }
    return NULL;
}

int dnet_adj_rx_hello(struct dnet_adjacency *sm, dnet_tick_t now,
                      const struct dnet_endnode_hello *h,
                      enum dnet_adj_state *state_out)
{
    if (!sm || !h)
        return DNET_ADJ_EINVAL;

    struct dnet_adj_neighbor *n = dnet_adj_find(sm, h->id);
    if (!n) {
        n = alloc_slot(sm);
        if (!n)
            return DNET_ADJ_EFULL;
        memset(n, 0, sizeof(*n));
        n->in_use = 1;
        memcpy(n->id, h->id, DNET_ADDR_LEN);
        n->addr  = dnet_addr_from_id(h->id);
        n->state = DNET_ADJ_DOWN;
        sm->count++;
    }

    /* A neighbour advertising timer 0 is nonsensical for the listen timer; fall
     * back to our own T3 so the adjacency does not arm a zero-length window. */
    uint16_t adv = h->timer ? h->timer : sm->t3;
    n->adv_t3          = adv;
    n->last_heard      = now;
    n->listen_deadline = now + (dnet_tick_t)sm->bct3mult * (dnet_tick_t)adv;

    /* Two-way reachability handshake (DNA Phase IV): the peer's HELLO names US
     * as its neighbour/designated router => it has heard us => the adjacency is
     * bidirectional. An all-zero neighbour field (0.0, "no router") is not us. */
    n->two_way = id_eq(h->neighbor, sm->my_id);

    switch (n->state) {
    case DNET_ADJ_DOWN:
        n->state = n->two_way ? DNET_ADJ_UP : DNET_ADJ_INITIALIZING;
        break;
    case DNET_ADJ_INITIALIZING:
        if (n->two_way)
            n->state = DNET_ADJ_UP;
        break;
    case DNET_ADJ_UP:
        /* Stay UP: a live HELLO refreshes the listen timer. Losing the two-way
         * bit while HELLOs still arrive is not by itself a teardown here; the
         * adjacency drops only when the listen timer lapses (dnet_adj_tick). */
        break;
    }

    if (state_out)
        *state_out = n->state;
    return DNET_ADJ_OK;
}

int dnet_adj_tick(struct dnet_adjacency *sm, dnet_tick_t now)
{
    if (!sm)
        return DNET_ADJ_EINVAL;

    int downed = 0;
    for (size_t i = 0; i < DNET_ADJ_MAX_NEIGHBORS; i++) {
        struct dnet_adj_neighbor *n = &sm->nbr[i];
        if (!n->in_use || n->state == DNET_ADJ_DOWN)
            continue;
        if (now >= n->listen_deadline) {
            n->state   = DNET_ADJ_DOWN;
            n->two_way = 0;
            downed++;
        }
    }
    return downed;
}

enum dnet_adj_state dnet_adj_state_of(const struct dnet_adjacency *sm,
                                      const uint8_t id[DNET_ADDR_LEN])
{
    if (!sm || !id)
        return DNET_ADJ_DOWN;
    for (size_t i = 0; i < DNET_ADJ_MAX_NEIGHBORS; i++) {
        if (sm->nbr[i].in_use && id_eq(sm->nbr[i].id, id))
            return sm->nbr[i].state;
    }
    return DNET_ADJ_DOWN;
}

int dnet_adj_hello_due(const struct dnet_adjacency *sm, dnet_tick_t now)
{
    if (!sm)
        return 0;
    return now >= sm->next_hello_due;
}

dnet_tick_t dnet_adj_next_hello_tick(const struct dnet_adjacency *sm)
{
    return sm ? sm->next_hello_due : 0;
}

void dnet_adj_hello_emitted(struct dnet_adjacency *sm, dnet_tick_t now)
{
    if (!sm)
        return;
    sm->next_hello_due = now + sm->t3;
}
