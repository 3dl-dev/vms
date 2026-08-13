/*
 * scs_membership.h - SCSD's live cluster-membership publication (vms-8d4).
 *
 * WHY THIS EXISTS. src/vmsscs/ implements the full SCS stack -- datalink,
 * NISCA VC, connection manager, MSCP serve, the vms-694 rejoin epic -- but all
 * of that live membership state has lived ONLY inside the SCSD process's
 * in-memory peer table. DCL SHOW CLUSTER (a SEPARATE userspace process) had no
 * way to read it, so cmd_show_cluster() hardcoded "%SYSTEM-I-NOTMEMBER" and a
 * genuinely-clustered node reported NOTMEMBER -- the facade INV-DCL exists to
 * kill.
 *
 * docs/design-cluster-node.md section 3.1 names the fix: SCSD "exposes a local
 * IPC (UNIX socket / shared memory) to the kernel modules and to userspace
 * consumers (SHOW CLUSTER, mount)". This header is the simplest honest form of
 * that IPC: SCSD PUBLISHES its live member set to a well-known file whenever
 * membership changes; consumers READ it. The file IS the daemon's real state,
 * not a per-process fake (Rule 9 / INV-6): when the daemon is not running or
 * this node has not joined, the file is absent and the honest answer is
 * NOTMEMBER -- which is exactly what a non-member VMS node reports.
 *
 * CLEAN-ROOM (Rule 8). This file's BYTE FORMAT is an OVMX INVENTION and is
 * labeled as such -- it is an internal OVMX IPC, not any VMS on-disk or wire
 * structure. Only the SHOW CLUSTER *display* fields it feeds (NODE / SOFTWARE /
 * STATUS, "View of Cluster from system ID N node: X") are matched to the
 * public OpenVMS SHOW CLUSTER utility output.
 *
 * ALL-STATIC-INLINE ON PURPOSE. SCSD and DCL are distinct link images; a shared
 * function symbol would have to be threaded through a shareable image's symbol
 * vector (CLAUDE.md native-link gotcha). Header-only static inline -- the same
 * pattern sysgen_params.h already uses -- keeps both images self-contained and
 * adds no cross-image symbol.
 */
#ifndef SCS_MEMBERSHIP_H
#define SCS_MEMBERSHIP_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* VMScluster tops out at 96 nodes; size the view to hold a full cluster. */
#define SCS_MEMBERSHIP_MAX_NODES   96
#define SCS_MEMBERSHIP_NODE_LEN    16   /* SCSNODE is <=6 chars; pad for safety */
#define SCS_MEMBERSHIP_STATE_LEN   16
#define SCS_MEMBERSHIP_VERSION     1

/*
 * One node as seen by the connection manager. `sysid` is the SCSSYSTEMID
 * (always known); `node` is the SCSNODE name when learned, else empty.
 */
struct scs_cluster_member {
    char     node[SCS_MEMBERSHIP_NODE_LEN];
    uint32_t sysid;
    char     state[SCS_MEMBERSHIP_STATE_LEN];   /* "MEMBER", "BRK_NON", ... */
};

/*
 * A point-in-time view of the cluster. n_members == 0 means "not a member"
 * (SHOW CLUSTER prints NOTMEMBER). Otherwise members[0..n_members-1] are the
 * SYSTEMS/MEMBERS the daemon currently sees, local node included.
 */
struct scs_cluster_view {
    int n_members;
    struct scs_cluster_member members[SCS_MEMBERSHIP_MAX_NODES];
};

/*
 * scs_membership_path - resolve the file both SCSD (writer) and SHOW CLUSTER
 * (reader) agree on. Overridable for tests and configurable per node:
 *   1. $OVMX_CLUSTER_STATE_PATH  -- explicit override (tests, custom layouts).
 *   2. "<$OVMX_SYSGEN_PATH>.members" -- keyed on the SYSGEN store, so it is
 *      keyed on the node identity the membership is ABOUT (same rule scsd's
 *      prior-admission ".cluster" sidecar uses). Both SCSD and DCL inherit this
 *      env from PID 1 on a real boot.
 *   3. "/var/run/ovmx/cluster_state" -- best-effort default; if it is not
 *      writable/present the reader simply finds no file and reports NOTMEMBER,
 *      which is honest.
 * Returns 0 on success, -1 if the resolved path did not fit.
 */
static inline int scs_membership_path(char *out, size_t out_len)
{
    const char *explicit_path = getenv("OVMX_CLUSTER_STATE_PATH");
    if (explicit_path != NULL && explicit_path[0] != '\0') {
        int n = snprintf(out, out_len, "%s", explicit_path);
        return (n > 0 && (size_t)n < out_len) ? 0 : -1;
    }
    const char *sysgen = getenv("OVMX_SYSGEN_PATH");
    if (sysgen != NULL && sysgen[0] != '\0') {
        int n = snprintf(out, out_len, "%s.members", sysgen);
        return (n > 0 && (size_t)n < out_len) ? 0 : -1;
    }
    int n = snprintf(out, out_len, "/var/run/ovmx/cluster_state");
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

/*
 * scs_membership_publish - SCSD writes the current view atomically (tmp+rename)
 * so a reader never sees a half-written file. Returns 0 on success, -1 on error.
 *
 * FORMAT (OVMX invention, Rule 8):
 *   version=1
 *   member=<node> <sysid> <state>
 *   ... (one line per member; local node included by the caller)
 */
static inline int scs_membership_publish(const struct scs_cluster_view *view)
{
    char path[1024];
    if (view == NULL || scs_membership_path(path, sizeof(path)) != 0) {
        return -1;
    }
    char tmp[1088];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
    if (tn <= 0 || (size_t)tn >= sizeof(tmp)) {
        return -1;
    }
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        return -1;
    }
    fprintf(f, "version=%d\n", SCS_MEMBERSHIP_VERSION);
    for (int i = 0; i < view->n_members && i < SCS_MEMBERSHIP_MAX_NODES; i++) {
        const struct scs_cluster_member *m = &view->members[i];
        const char *node  = (m->node[0] != '\0')  ? m->node  : "?";
        const char *state = (m->state[0] != '\0') ? m->state : "MEMBER";
        fprintf(f, "member=%s %u %s\n", node, (unsigned)m->sysid, state);
    }
    if (fflush(f) != 0 || fclose(f) != 0) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/*
 * scs_membership_clear - SCSD removes the file when this node is no longer a
 * member (departed, or shutting down). A subsequent SHOW CLUSTER then finds no
 * file and reports NOTMEMBER, which is the truth.
 */
static inline void scs_membership_clear(void)
{
    char path[1024];
    if (scs_membership_path(path, sizeof(path)) == 0) {
        remove(path);
    }
}

/*
 * scs_membership_read - a consumer (SHOW CLUSTER, F$GETSYI) reads the published
 * view. Returns:
 *    1  file present and parsed, out->n_members set (>=0);
 *    0  no file -- this node is not currently a cluster member (NOTMEMBER);
 *   -1  a bad argument.
 * A parse that finds zero member lines yields n_members == 0, also NOTMEMBER.
 */
static inline int scs_membership_read(struct scs_cluster_view *out)
{
    char path[1024];
    if (out == NULL || scs_membership_path(path, sizeof(path)) != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;   /* no published membership -> NOTMEMBER */
    }
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        char node[SCS_MEMBERSHIP_NODE_LEN];
        char state[SCS_MEMBERSHIP_STATE_LEN];
        unsigned sysid = 0;
        node[0] = '\0';
        state[0] = '\0';
        if (sscanf(line, "member=%15s %u %15s", node, &sysid, state) >= 2) {
            if (out->n_members >= SCS_MEMBERSHIP_MAX_NODES) {
                break;
            }
            struct scs_cluster_member *m = &out->members[out->n_members++];
            strncpy(m->node, node, sizeof(m->node) - 1);
            m->node[sizeof(m->node) - 1] = '\0';
            m->sysid = sysid;
            if (state[0] == '\0') {
                strncpy(m->state, "MEMBER", sizeof(m->state) - 1);
            } else {
                strncpy(m->state, state, sizeof(m->state) - 1);
            }
            m->state[sizeof(m->state) - 1] = '\0';
        }
    }
    fclose(f);
    return 1;
}

#endif /* SCS_MEMBERSHIP_H */
