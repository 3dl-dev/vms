/*
 * test_lookup_integration.c - proves the KFE database INSTALL writes is
 * loadable by the standalone known_images lookup module (bead vms-913.5).
 *
 * Not a self-contained test: invoked by run_install_test.sh with the real
 * on-disk DB path and a SONAME to look up, after INSTALL ADD has run.
 * This is the cross-tool half of the IPC design (mmap MAP_SHARED over the
 * page cache — no daemon, no socket) described in
 * src/imgact/known_images.h and docs/design-image-activation.md section 6.
 *
 * Usage: test_lookup_integration <db-path> <soname>
 * Exit 0 and prints "FOUND: ..." if the lookup succeeds, 1 otherwise.
 */
#include "known_images.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <db-path> <soname>\n", argv[0]);
        return 2;
    }

    struct known_images_db db;
    if (known_images_open(&db, argv[1]) != 0) {
        fprintf(stderr, "FAIL: known_images_open(%s) failed\n", argv[1]);
        return 1;
    }

    const struct kfe_entry *e = known_images_lookup(&db, argv[2]);
    if (!e) {
        fprintf(stderr, "FAIL: %s not found in %s\n", argv[2], argv[1]);
        known_images_close(&db);
        return 1;
    }

    printf("FOUND: %s -> %s (flags=0x%x)\n", argv[2], e->path, e->flags);
    known_images_close(&db);
    return 0;
}
