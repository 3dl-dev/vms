/*
 * test_dnet_objectdb.c - DECnet Phase IV OBJECT database unit tests (rd vms-f52).
 *
 * Covers SET/DEFINE OBJECT add + update (keyed by number), object-name
 * uniqueness, name/number resolution, FILE handling + bounds, CLEAR/PURGE,
 * SHOW ordering (ascending number), and persistence round-trip / corrupt-file
 * handling. Pure logic + a temp file; no socket, no privilege, no /dev/vms.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dnet_objectdb.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

int main(void)
{
    printf("test_dnet_objectdb: DECnet Phase IV object database\n");

    /* --- set / update / uniqueness (keyed by NUMBER) --- */
    struct dnet_objectdb db;
    dnet_objectdb_init(&db);
    check(dnet_objectdb_set(&db, 42, "CTERM", "SYS$SYSTEM:DECNETD.EXE") == DNET_OBJECTDB_OK,
          "SET OBJECT CTERM NUMBER 42 FILE ...");
    check(dnet_objectdb_set(&db, 17, "FAL", "SYS$SYSTEM:DECNETD.EXE") == DNET_OBJECTDB_OK,
          "SET OBJECT FAL NUMBER 17");
    check(dnet_objectdb_set(&db, 25, "", "") == DNET_OBJECTDB_OK,
          "SET OBJECT NUMBER 25 (no name/file)");
    check(db.count == 3, "count == 3");
    /* Re-SET an existing number updates its name + file (no dup). */
    check(dnet_objectdb_set(&db, 25, "TASK", "") == DNET_OBJECTDB_OK,
          "update object 25 -> NAME TASK");
    check(db.count == 3, "count still 3 after update");
    /* A name already bound to a different number is rejected. */
    check(dnet_objectdb_set(&db, 99, "CTERM", "") == DNET_OBJECTDB_EINVAL,
          "reject CTERM on a second number");
    /* number 0 is invalid. */
    check(dnet_objectdb_set(&db, 0, "ZERO", "") == DNET_OBJECTDB_EINVAL,
          "reject object number 0");

    /* --- name normalisation + bounds --- */
    check(dnet_objectdb_set(&db, 60, "mail", "") == DNET_OBJECTDB_OK,
          "lowercase name accepted (normalised to uppercase)");
    check(dnet_objectdb_by_name(&db, "MAIL") != NULL &&
          dnet_objectdb_by_name(&db, "mail") != NULL,
          "name lookup is case-insensitive");
    check(dnet_objectdb_by_name(&db, "MAIL")->number == 60, "MAIL resolves to 60");
    check(dnet_objectdb_set(&db, 61, "BAD NAME", "") == DNET_OBJECTDB_EINVAL,
          "reject name with a space");
    check(dnet_objectdb_set(&db, 61, "TOOLONGOBJECTNAME1", "") == DNET_OBJECTDB_EINVAL,
          "reject name > 16 chars");
    check(dnet_objectdb_set(&db, 61, "OK$_1", "") == DNET_OBJECTDB_OK,
          "accept name with $ and _");
    /* FILE bounds: a spec with whitespace is rejected (line format is token-based). */
    check(dnet_objectdb_set(&db, 62, "SP", "has space") == DNET_OBJECTDB_EINVAL,
          "reject FILE with a space");

    /* --- number/name lookups --- */
    check(dnet_objectdb_by_number(&db, 42) != NULL &&
          strcmp(dnet_objectdb_by_number(&db, 42)->name, "CTERM") == 0,
          "by_number(42) == CTERM");
    check(dnet_objectdb_by_number(&db, 200) == NULL, "by_number(200) absent -> NULL");
    check(dnet_objectdb_by_name(&db, "NOSUCH") == NULL, "by_name(NOSUCH) -> NULL");

    /* --- SHOW ordering: ascending by number --- */
    {
        struct dnet_objectdb o;
        dnet_objectdb_init(&o);
        dnet_objectdb_set(&o, 42, "CTERM", "");
        dnet_objectdb_set(&o, 17, "FAL", "");
        dnet_objectdb_set(&o, 25, "TASK", "");
        const struct dnet_object_entry *e0 = dnet_objectdb_at(&o, 0);
        const struct dnet_object_entry *e1 = dnet_objectdb_at(&o, 1);
        const struct dnet_object_entry *e2 = dnet_objectdb_at(&o, 2);
        check(e0 && e1 && e2 && e0->number == 17 && e1->number == 25 && e2->number == 42,
              "at() yields ascending number order 17,25,42");
        check(dnet_objectdb_at(&o, 3) == NULL, "at() past end -> NULL");
    }

    /* --- CLEAR / PURGE --- */
    check(dnet_objectdb_clear_number(&db, 17) == DNET_OBJECTDB_OK, "CLEAR OBJECT 17");
    check(dnet_objectdb_by_number(&db, 17) == NULL, "17 gone after clear");
    check(dnet_objectdb_clear_number(&db, 17) == DNET_OBJECTDB_ENOENT,
          "CLEAR OBJECT 17 again -> ENOENT");
    check(dnet_objectdb_clear_name(&db, "CTERM") == DNET_OBJECTDB_OK, "PURGE OBJECT CTERM");
    check(dnet_objectdb_by_number(&db, 42) == NULL, "42 gone after clear-by-name");
    check(dnet_objectdb_clear_name(&db, "NOSUCH") == DNET_OBJECTDB_ENOENT,
          "clear NOSUCH -> ENOENT");

    /* --- persistence round-trip --- */
    {
        char path[] = "/tmp/ovmx_objdb_test_XXXXXX";
        int fd = mkstemp(path);
        check(fd >= 0, "mkstemp for object DB");
        if (fd >= 0)
            close(fd);
        struct dnet_objectdb a, b;
        dnet_objectdb_init(&a);
        dnet_objectdb_set(&a, 42, "CTERM", "SYS$SYSTEM:DECNETD.EXE");
        dnet_objectdb_set(&a, 17, "FAL", "");
        dnet_objectdb_set(&a, 25, "", "");
        check(dnet_objectdb_save(&a, path) == DNET_OBJECTDB_OK, "save object DB");
        check(dnet_objectdb_load(&b, path) == DNET_OBJECTDB_OK, "load object DB");
        check(b.count == 3, "loaded count == 3");
        const struct dnet_object_entry *c = dnet_objectdb_by_number(&b, 42);
        check(c && strcmp(c->name, "CTERM") == 0 &&
              strcmp(c->file, "SYS$SYSTEM:DECNETD.EXE") == 0,
              "42 round-trips name + file");
        const struct dnet_object_entry *f = dnet_objectdb_by_number(&b, 17);
        check(f && strcmp(f->name, "FAL") == 0 && f->file[0] == '\0',
              "17 round-trips name, empty file");
        const struct dnet_object_entry *n25 = dnet_objectdb_by_number(&b, 25);
        check(n25 && n25->name[0] == '\0' && n25->file[0] == '\0',
              "25 round-trips as number-only");
        remove(path);
    }

    /* --- absent file loads as empty (not an error) --- */
    {
        struct dnet_objectdb e;
        check(dnet_objectdb_load(&e, "/tmp/ovmx_objdb_does_not_exist_zzz") == DNET_OBJECTDB_OK &&
              e.count == 0, "absent DB loads as empty, not an error");
    }

    /* --- corrupt file is rejected (EIO), DB left empty --- */
    {
        char path[] = "/tmp/ovmx_objdb_bad_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            const char *junk = "OBJECT notanumber NAME X\n";
            ssize_t w = write(fd, junk, strlen(junk));
            (void)w;
            close(fd);
        }
        struct dnet_objectdb e;
        check(dnet_objectdb_load(&e, path) == DNET_OBJECTDB_EIO && e.count == 0,
              "corrupt line -> EIO, DB left empty");
        remove(path);
    }

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
