/*
 * scs_quorum.c - CEVOTES/QUORUM computation and the quorum gate (vms-7a9).
 *
 * The algorithm is the public-documented connection-manager quorum recomputation
 * (VMScluster Systems for OpenVMS sec 2.3.5; the mined CM transcript ch7-part01
 * pp. 7-5..7-7 carries the identical CEVOTES form + the 5-node worked example the
 * unit test reproduces). See scs_quorum.h for the full grounding statement and
 * the clean-room note (rule 8): the arithmetic is documented, only the per-node
 * VOTES field is wire-grounded.
 */
#include "scs_quorum.h"

#include <string.h>

void scs_quorum_init(struct scs_quorum *q)
{
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof(*q));
}

static struct scs_quorum_member *find_member(struct scs_quorum *q,
                                             uint32_t node_id)
{
    for (size_t i = 0; i < q->n_members; i++) {
        if (q->members[i].node_id == node_id) {
            return &q->members[i];
        }
    }
    return NULL;
}

int scs_quorum_set_member(struct scs_quorum *q, uint32_t node_id,
                          uint16_t votes, uint16_t expected_votes, int up)
{
    if (q == NULL) {
        return -1;
    }
    struct scs_quorum_member *m = find_member(q, node_id);
    if (m == NULL) {
        if (q->n_members >= SCS_QUORUM_MAX_MEMBERS) {
            return -1;
        }
        m = &q->members[q->n_members++];
        m->node_id = node_id;
    }
    m->votes = votes;
    m->expected_votes = expected_votes;
    m->up = up ? 1 : 0;
    return 0;
}

int scs_quorum_set_up(struct scs_quorum *q, uint32_t node_id, int up)
{
    if (q == NULL) {
        return -1;
    }
    struct scs_quorum_member *m = find_member(q, node_id);
    if (m == NULL) {
        return -1;
    }
    m->up = up ? 1 : 0;
    return 0;
}

void scs_quorum_set_disk(struct scs_quorum *q, uint32_t qdskvotes, int present)
{
    if (q == NULL) {
        return;
    }
    q->quorum_disk_votes = qdskvotes;
    q->quorum_disk_present = present ? 1 : 0;
}

void scs_quorum_recompute(struct scs_quorum *q)
{
    if (q == NULL) {
        return;
    }

    /* SUM VOTES over ALL members (history high-water term) and the quorum disk;
     * the largest single EXPECTED_VOTES; and PRESENT votes over UP members. */
    uint32_t sum_votes_all = 0;
    uint32_t max_expected = 0;
    uint32_t present = 0;

    for (size_t i = 0; i < q->n_members; i++) {
        const struct scs_quorum_member *m = &q->members[i];
        sum_votes_all += m->votes;
        if (m->expected_votes > max_expected) {
            max_expected = m->expected_votes;
        }
        if (m->up) {
            present += m->votes;
        }
    }
    if (q->quorum_disk_present) {
        /* A configured quorum disk is a virtual member: it raises the vote
         * total (VMScluster Systems sec 2.3.8) and, when watched/up, present. */
        sum_votes_all += q->quorum_disk_votes;
        present += q->quorum_disk_votes;
    }

    /* New CEVOTES = max{ max EXPECTED_VOTES ; SUM VOTES ; Old CEVOTES }. The
     * Old-CEVOTES term is why the connection manager NEVER decreases the value
     * on its own (sec 2.3.5 Note). */
    uint32_t new_cevotes = q->cevotes; /* Old CEVOTES */
    if (max_expected > new_cevotes) {
        new_cevotes = max_expected;
    }
    if (sum_votes_all > new_cevotes) {
        new_cevotes = sum_votes_all;
    }

    q->cevotes = new_cevotes;
    q->quorum = (new_cevotes + 2) / 2; /* integer division = "rounded down" */
    q->present_votes = present;
}

int scs_quorum_present(const struct scs_quorum *q)
{
    if (q == NULL) {
        return 0;
    }
    return q->present_votes >= q->quorum;
}

enum scs_quorum_gate_result scs_quorum_gate(const struct scs_quorum *q)
{
    return scs_quorum_present(q) ? SCS_QUORUM_RUN : SCS_QUORUM_BLOCK;
}
