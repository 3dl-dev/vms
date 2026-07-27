/*
 * test_known_images.c - unit test for the standalone known-image lookup
 * module (bead vms-913.5). Builds a KFE database file directly (no
 * dependency on the INSTALL utility), then proves known_images_open()/
 * known_images_lookup() find registered images by SONAME via mmap.
 *
 * Exit 0 on success, 1 on first failure.
 */
#define _POSIX_C_SOURCE 200809L

#include "known_images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("PASS: %s\n", msg); \
    } else { \
        printf("FAIL: %s\n", msg); \
        failures++; \
    } \
} while (0)

static void write_test_db(const char *path)
{
    struct kfe_file db;
    memset(&db, 0, sizeof(db));
    db.magic = KFE_MAGIC;
    db.version = KFE_VERSION;
    db.count = 2;

    strncpy(db.entries[0].soname, "LIBVMS$SHR.EXE", sizeof(db.entries[0].soname) - 1);
    strncpy(db.entries[0].path, "/vms/SYS0/SYSCOMMON/SYSLIB/LIBVMS$SHR.EXE",
            sizeof(db.entries[0].path) - 1);
    db.entries[0].flags = KFE_F_OPEN | KFE_F_SHARED | KFE_F_HEADER_RESIDENT;
    db.entries[0].gsmatch_op = KFE_GSMATCH_LEQUAL;
    db.entries[0].major = 1;
    db.entries[0].minor = 0;

    strncpy(db.entries[1].soname, "LIBVMSFS$SHR.EXE", sizeof(db.entries[1].soname) - 1);
    strncpy(db.entries[1].path, "/vms/SYS0/SYSCOMMON/SYSLIB/LIBVMSFS$SHR.EXE",
            sizeof(db.entries[1].path) - 1);
    db.entries[1].flags = KFE_F_OPEN | KFE_F_SHARED;
    db.entries[1].gsmatch_op = KFE_GSMATCH_ALWAYS;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("fopen(write_test_db)");
        exit(1);
    }
    if (fwrite(&db, sizeof(db), 1, fp) != 1) {
        perror("fwrite(write_test_db)");
        fclose(fp);
        exit(1);
    }
    fclose(fp);
}

int main(void)
{
    char path[] = "/tmp/known_images_test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);

    write_test_db(path);

    struct known_images_db db;
    int rc = known_images_open(&db, path);
    CHECK(rc == 0, "known_images_open succeeds on a valid KFE file");
    CHECK(known_images_count(&db) == 2, "known_images_count reports 2 entries");

    const struct kfe_entry *e = known_images_lookup(&db, "LIBVMS$SHR.EXE");
    CHECK(e != NULL, "lookup finds LIBVMS$SHR.EXE by SONAME");
    if (e) {
        CHECK(strcmp(e->path, "/vms/SYS0/SYSCOMMON/SYSLIB/LIBVMS$SHR.EXE") == 0,
              "found entry has the expected resolved path");
        CHECK((e->flags & KFE_F_OPEN) && (e->flags & KFE_F_SHARED) &&
              (e->flags & KFE_F_HEADER_RESIDENT),
              "found entry carries OPEN|SHARED|HEADER_RESIDENT flags");
        CHECK(e->gsmatch_op == KFE_GSMATCH_LEQUAL && e->major == 1 && e->minor == 0,
              "found entry carries cached GSMATCH LEQUAL,1,0");
    }

    const struct kfe_entry *e2 = known_images_lookup(&db, "LIBVMSFS$SHR.EXE");
    CHECK(e2 != NULL, "lookup finds the second registered image");

    const struct kfe_entry *miss = known_images_lookup(&db, "NOTINSTALLED$SHR.EXE");
    CHECK(miss == NULL, "lookup returns NULL for an unregistered SONAME");

    known_images_close(&db);
    CHECK(known_images_lookup(&db, "LIBVMS$SHR.EXE") == NULL,
          "lookup on a closed db returns NULL (no dangling mmap use)");

    /* Open on a nonexistent path must fail cleanly (this is what lets an
     * IMGACT-side caller fall back to filesystem search per the design). */
    struct known_images_db missing_db;
    rc = known_images_open(&missing_db, "/tmp/known_images_test_does_not_exist.dat");
    CHECK(rc != 0, "known_images_open fails cleanly on a missing DB file");

    unlink(path);

    if (failures == 0) {
        printf("ALL known_images UNIT TESTS PASSED\n");
        return 0;
    }
    printf("%d known_images UNIT TEST(S) FAILED\n", failures);
    return 1;
}
