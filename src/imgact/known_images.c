/*
 * known_images.c - Known Image Database lookup module (bead vms-913.5).
 *
 * See known_images.h for the on-disk format and the IPC design (mmap
 * MAP_SHARED as the change-propagation channel between INSTALL and this
 * module). This file implements only the reader side.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "known_images.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* FNV-1a — simple, dependency-free, adequate for a 256-slot table. */
static uint32_t soname_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void build_index(struct known_images_db *db)
{
    for (int i = 0; i < KFE_HASH_SIZE; i++)
        db->hash_slot[i] = -1;

    uint32_t count = db->file->count;
    if (count > KFE_MAX_IMAGES)
        count = KFE_MAX_IMAGES; /* defensive; open() already rejects this */

    for (uint32_t i = 0; i < count; i++) {
        /* soname[64] may not be NUL-terminated if a caller wrote exactly
         * 64 bytes; hash/compare bounded by sizeof(soname) throughout. */
        char key[sizeof(db->file->entries[0].soname) + 1];
        memcpy(key, db->file->entries[i].soname, sizeof(db->file->entries[0].soname));
        key[sizeof(key) - 1] = '\0';

        uint32_t slot = soname_hash(key) & (KFE_HASH_SIZE - 1);
        for (int probe = 0; probe < KFE_HASH_SIZE; probe++) {
            uint32_t s = (slot + (uint32_t)probe) & (KFE_HASH_SIZE - 1);
            if (db->hash_slot[s] == -1) {
                db->hash_slot[s] = (int)i;
                break;
            }
        }
    }
}

int known_images_open(struct known_images_db *db, const char *path)
{
    if (!db || !path)
        return -1;

    memset(db, 0, sizeof(*db));
    db->fd = -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    if ((size_t)st.st_size != sizeof(struct kfe_file)) {
        /* Not our format (or a partial/corrupt write in progress). */
        close(fd);
        return -1;
    }

    void *map = mmap(NULL, sizeof(struct kfe_file), PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct kfe_file *file = (struct kfe_file *)map;
    if (file->magic != KFE_MAGIC || file->version != KFE_VERSION ||
        file->count > KFE_MAX_IMAGES) {
        munmap(map, sizeof(struct kfe_file));
        close(fd);
        return -1;
    }

    db->fd = fd;
    db->map_base = map;
    db->map_len = sizeof(struct kfe_file);
    db->file = file;

    build_index(db);
    return 0;
}

void known_images_close(struct known_images_db *db)
{
    if (!db)
        return;
    if (db->map_base && db->map_base != MAP_FAILED)
        munmap(db->map_base, db->map_len);
    if (db->fd >= 0)
        close(db->fd);
    memset(db, 0, sizeof(*db));
    db->fd = -1;
}

const struct kfe_entry *known_images_lookup(const struct known_images_db *db,
                                             const char *soname)
{
    if (!db || !db->file || !soname)
        return NULL;

    char key[sizeof(db->file->entries[0].soname)];
    memset(key, 0, sizeof(key));
    strncpy(key, soname, sizeof(key) - 1);

    uint32_t slot = soname_hash(key) & (KFE_HASH_SIZE - 1);
    for (int probe = 0; probe < KFE_HASH_SIZE; probe++) {
        uint32_t s = (slot + (uint32_t)probe) & (KFE_HASH_SIZE - 1);
        int idx = db->hash_slot[s];
        if (idx == -1)
            return NULL; /* open-addressing: empty slot terminates the probe chain */
        const struct kfe_entry *e = &db->file->entries[idx];
        if (strncmp(e->soname, key, sizeof(e->soname)) == 0)
            return e;
    }
    return NULL;
}

int known_images_count(const struct known_images_db *db)
{
    if (!db || !db->file)
        return -1;
    return (int)db->file->count;
}
