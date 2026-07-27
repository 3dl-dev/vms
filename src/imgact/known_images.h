/*
 * known_images.h - Known Image Database (KFE) format + standalone lookup
 * module (bead vms-913.5).
 *
 * On-disk format for SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT, matching
 * docs/design-image-activation.md section 6 ("INSTALL Utility and Known
 * Image Database"). The database is a single fixed-size binary file — a
 * struct kfe_file header followed by a fixed KFE_MAX_IMAGES-entry array
 * (same whole-file-struct idiom as SYSGEN's sysgen_params.h) — so it can
 * be mmap()'d directly with no variable-length parsing.
 *
 * Writer:  src/install/install.c   (INSTALL ADD/LIST/REMOVE, plain
 *          buffered fread()/fwrite() of the whole struct kfe_file)
 * Reader:  this module             (mmap(MAP_SHARED), O(1) hash lookup
 *          by SONAME for IMGACT.EXE's known-image search path)
 *
 * IPC between INSTALL and the reader is the shared page cache itself:
 * known_images_open() mmaps the file MAP_SHARED, so a process that reopens
 * the mapping after INSTALL ADD/REMOVE sees the change immediately with no
 * daemon, socket, or notification channel (docs/design-image-activation.md
 * section "IPC Between INSTALL and IMGACT.EXE", option (a)).
 *
 * NOTE: wiring known_images_lookup() into src/imgact/imgact.c's activation
 * search path is a deliberate follow-up, NOT done by this module. imgact.c
 * is owned by a concurrent bead (vms-913.11, x86_64 IMGACT support); see the
 * escalation on vms-913.5. This header/module is self-contained and has its
 * own unit test (src/imgact/test/test_known_images.c) that proves the mmap
 * lookup works against a hand-built database file.
 */
#ifndef OVMX_KNOWN_IMAGES_H
#define OVMX_KNOWN_IMAGES_H

#include <stddef.h>
#include <stdint.h>

#define KFE_MAGIC       0x4B464521u  /* "KFE!" */
#define KFE_VERSION     1u
#define KFE_MAX_IMAGES  256
#define KFE_HASH_SIZE   512          /* power of 2, load factor < 0.5 for 256 max entries */

/* kfe_entry.flags */
#define KFE_F_OPEN             0x0001u  /* /OPEN  — keep fd cached */
#define KFE_F_SHARED           0x0002u  /* /SHARED — mmap(MAP_SHARED), kernel shares physical pages */
#define KFE_F_HEADER_RESIDENT  0x0004u  /* /HEADER_RESIDENT — cache ELF headers, skip disk read */

/* kfe_entry.gsmatch_op — mirrors struct vms_ident.gsmatch_op,
 * docs/design-image-activation.md section 3 ("VMS Image Identity"). */
#define KFE_GSMATCH_ALWAYS  0  /* any version accepted */
#define KFE_GSMATCH_EQUAL   1  /* exact match required */
#define KFE_GSMATCH_LEQUAL  2  /* actual >= linked (default) */

/*
 * struct kfe_entry - one known-image database record.
 *
 * image_name (SONAME) is the hash key; image_path is the resolved Linux
 * filesystem path. major/minor/gsmatch_op are cached GSMATCH version info
 * (docs/design-image-activation.md section 5) — populated from a producing
 * image's .vms.ident section when a linker emits one; no such producer
 * exists yet in this tree, so INSTALL currently stores GSMATCH_ALWAYS/0/0
 * (see src/install/install.c). The fields exist so the on-disk format does
 * not need to change when a producer lands.
 */
struct kfe_entry {
    char     soname[64];    /* image_name: SONAME, NUL-terminated, hash key */
    char     path[256];     /* image_path: resolved Linux path to the .EXE file */
    uint32_t flags;         /* KFE_F_* */
    uint16_t major;         /* cached GSMATCH major version */
    uint16_t minor;         /* cached GSMATCH minor version */
    uint8_t  gsmatch_op;    /* cached GSMATCH operator, KFE_GSMATCH_* */
    uint8_t  reserved[7];
};

/*
 * struct kfe_file - the entire on-disk database, mmap'd/fread as one unit.
 * File size is always sizeof(struct kfe_file); entries[0..count-1] are the
 * live records, entries[count..KFE_MAX_IMAGES-1] are unused/zeroed.
 */
struct kfe_file {
    uint32_t magic;       /* KFE_MAGIC */
    uint32_t version;     /* KFE_VERSION */
    uint32_t count;       /* number of valid entries */
    uint32_t reserved;
    struct kfe_entry entries[KFE_MAX_IMAGES];
};

/* ------------------------------------------------------------------ */
/* Standalone lookup module — mmap(MAP_SHARED), O(1) hash lookup       */
/* ------------------------------------------------------------------ */

struct known_images_db {
    int               fd;
    void             *map_base;   /* mmap base; == (void *)file */
    size_t            map_len;    /* == sizeof(struct kfe_file) */
    struct kfe_file  *file;
    int               hash_slot[KFE_HASH_SIZE]; /* -1 = empty, else entries[] index */
};

/*
 * known_images_open - mmap(MAP_SHARED) the known-image DB at 'path' and
 * build an in-memory open-addressing hash index over its entries (the
 * index itself is process-local scratch space; only the mmap'd entries
 * array is the shared/authoritative state).
 *
 * Returns 0 on success, -1 on failure (file missing, wrong size, bad
 * magic/version, too many entries). Callers should treat -1 as "no known-
 * image database available" and fall back to filesystem search, per
 * docs/design-image-activation.md section 4 (Priority 2/3/4) — this module
 * never itself does a filesystem search.
 */
int known_images_open(struct known_images_db *db, const char *path);

/* known_images_close - unmap and close. Safe to call on a zero-initialized
 * or already-closed db (idempotent). */
void known_images_close(struct known_images_db *db);

/*
 * known_images_lookup - O(1) lookup by SONAME (exact, case-sensitive match,
 * matching how DT_NEEDED/SONAME strings are compared elsewhere in the
 * activation path). Returns a pointer into the mmap'd region — valid until
 * known_images_close() — or NULL if not found or db is not open.
 */
const struct kfe_entry *known_images_lookup(const struct known_images_db *db,
                                             const char *soname);

/* known_images_count - number of live entries, or -1 if db is not open. */
int known_images_count(const struct known_images_db *db);

#endif /* OVMX_KNOWN_IMAGES_H */
