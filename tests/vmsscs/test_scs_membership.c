/*
 * test_scs_membership.c - unit test for the SCSD<->consumer membership IPC
 * (vms-8d4, scs_membership.h).
 *
 * This exercises the REAL boundary the SHOW CLUSTER facade-kill turns on: the
 * publish/read round-trip that SCSD (writer) and DCL SHOW CLUSTER / F$GETSYI
 * (readers) share. It does NOT mock either side -- it calls the actual
 * scs_membership_publish()/scs_membership_read()/scs_membership_clear() and
 * checks the file contract:
 *   - a published member set reads back byte-for-field identical;
 *   - an absent file reads as "not a member" (rc 0, n_members 0 -> NOTMEMBER);
 *   - clear() removes the file, restoring the NOTMEMBER answer.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "scs_membership.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); failures++; } \
    else { printf("  ok: %s\n", (msg)); } \
} while (0)

int main(void)
{
    /* Point the IPC at a private temp file so the test is hermetic. */
    char tmpl[] = "/tmp/ovmx_scs_membership_XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);
    remove(tmpl);   /* start with NO file -> the standalone case */
    setenv("OVMX_CLUSTER_STATE_PATH", tmpl, 1);
    /* OVMX_SYSGEN_PATH must not shadow the explicit override. */
    unsetenv("OVMX_SYSGEN_PATH");

    printf("vms-8d4 scs_membership IPC unit test\n");

    /* --- absent file -> NOTMEMBER ------------------------------------- */
    struct scs_cluster_view view;
    int rc = scs_membership_read(&view);
    CHECK(rc == 0, "absent file reads rc 0 (no published membership)");
    CHECK(view.n_members == 0, "absent file yields 0 members (NOTMEMBER)");

    /* --- publish a two-node member set and read it back --------------- */
    struct scs_cluster_view pub;
    memset(&pub, 0, sizeof(pub));
    strncpy(pub.members[0].node, "VAX3", sizeof(pub.members[0].node) - 1);
    pub.members[0].sysid = 1027;
    strncpy(pub.members[0].state, "MEMBER", sizeof(pub.members[0].state) - 1);
    strncpy(pub.members[1].node, "VAX1", sizeof(pub.members[1].node) - 1);
    pub.members[1].sysid = 1025;
    strncpy(pub.members[1].state, "MEMBER", sizeof(pub.members[1].state) - 1);
    pub.n_members = 2;

    rc = scs_membership_publish(&pub);
    CHECK(rc == 0, "publish of a two-node member set succeeds");

    memset(&view, 0, sizeof(view));
    rc = scs_membership_read(&view);
    CHECK(rc == 1, "published file reads rc 1 (present)");
    CHECK(view.n_members == 2, "two members read back");
    CHECK(view.members[0].sysid == 1027 &&
          strcmp(view.members[0].node, "VAX3") == 0 &&
          strcmp(view.members[0].state, "MEMBER") == 0,
          "local node (VAX3/1027/MEMBER) round-trips");
    CHECK(view.members[1].sysid == 1025 &&
          strcmp(view.members[1].node, "VAX1") == 0,
          "peer node (VAX1/1025) round-trips");

    /* --- an unknown-name peer publishes as "?" and reads back keyed on
     *     its SCSSYSTEMID (SHOW CLUSTER renders the sysid) ------------- */
    memset(&pub, 0, sizeof(pub));
    strncpy(pub.members[0].node, "VAX3", sizeof(pub.members[0].node) - 1);
    pub.members[0].sysid = 1027;
    strncpy(pub.members[0].state, "MEMBER", sizeof(pub.members[0].state) - 1);
    /* members[1].node left empty -> publish writes "?" */
    pub.members[1].sysid = 1026;
    strncpy(pub.members[1].state, "MEMBER", sizeof(pub.members[1].state) - 1);
    pub.n_members = 2;
    rc = scs_membership_publish(&pub);
    CHECK(rc == 0, "publish with an unknown-name peer succeeds");
    memset(&view, 0, sizeof(view));
    rc = scs_membership_read(&view);
    CHECK(rc == 1 && view.n_members == 2 &&
          view.members[1].sysid == 1026 &&
          strcmp(view.members[1].node, "?") == 0,
          "unknown-name peer reads back as '?' keyed on sysid 1026");

    /* --- clear() removes the file -> back to NOTMEMBER ---------------- */
    scs_membership_clear();
    memset(&view, 0, sizeof(view));
    rc = scs_membership_read(&view);
    CHECK(rc == 0 && view.n_members == 0,
          "after clear(), read is NOTMEMBER again");

    remove(tmpl);

    if (failures == 0) {
        printf("vms-8d4 scs_membership IPC unit test: PASS\n");
        return 0;
    }
    printf("vms-8d4 scs_membership IPC unit test: FAIL (%d)\n", failures);
    return 1;
}
