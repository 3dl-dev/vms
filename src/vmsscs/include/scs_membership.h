/*
 * scs_membership.h - shared cluster-membership view struct (vms-8d4).
 *
 * HISTORY. This header used to carry a userspace-to-userspace IPC: SCSD
 * (writer) PUBLISHED its live member set to a well-known file
 * (/var/run/ovmx/cluster_state) and DCL SHOW CLUSTER (reader) read it, because
 * the connection manager's live membership state lived only inside the SCSD
 * process's in-memory peer table and had no other way out.
 *
 * vms-551 (docs/design-cluster-membership-executive.md) crossed cluster
 * membership into the executive: vms.ko now owns a membership block that
 * SCSD populates via VMS_IOCTL_CLUSTER_MEMBER_SET/CLEAR, and readers get it
 * back through /dev/vms (vms_kif_cluster_get_members()). SHOW CLUSTER
 * (dcl_cmd_show.c) and F$GETSYI CLUSTER_MEMBER/CLUSTER_NODES (vms-5919,
 * src/libvms/syssvc/sys_misc.c) both cut over to that executive read.
 *
 * vms-967d RETIRED the file bridge itself once both readers had moved off it:
 * scs_membership_path()/_publish()/_read()/_clear() are gone. What remains is
 * the plain struct contract scsd.c still uses internally to stage the member
 * set it hands to the executive SET ioctl -- this header is a struct-only
 * definition now, not an IPC.
 *
 * CLEAN-ROOM (Rule 8). These structs are an OVMX-internal staging shape, not
 * any VMS on-disk or wire structure. Only the SHOW CLUSTER *display* fields
 * they ultimately feed (NODE / SOFTWARE / STATUS, "View of Cluster from
 * system ID N node: X") are matched to the public OpenVMS SHOW CLUSTER
 * utility output.
 */
#ifndef SCS_MEMBERSHIP_H
#define SCS_MEMBERSHIP_H

#include <stdint.h>

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

#endif /* SCS_MEMBERSHIP_H */
