/*
 * lib_vm.c - VMS Zone-Based Memory Allocator
 *
 * Full implementation of the VMS LIB$*_VM routines with zone support:
 *
 *   LIB$GET_VM       - Allocate from a zone (or default zone 0)
 *   LIB$FREE_VM      - Free to a zone
 *   LIB$CREATE_VM_ZONE - Create an isolated memory zone
 *   LIB$DELETE_VM_ZONE - Bulk-free an entire zone
 *   LIB$GET_VM_PAGE  - Allocate in 512-byte page units
 *   LIB$FREE_VM_PAGE - Free page-unit allocations
 *
 * Architecture:
 *   - Each zone is backed by one or more mmap'd regions ("extents")
 *   - Zone 0 (default) uses quick-fit with lookaside lists for
 *     small allocations (8/16/32/64/128/256/512/1024/2048 bytes)
 *   - Large allocations (>2048) get their own mmap region
 *   - LIB$DELETE_VM_ZONE bulk-frees by munmap'ing all extents
 *   - Thread-safe via pthread mutex per zone
 *
 * VMS page size = 512 bytes (not Linux page size).
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

/* ================================================================
 * Configuration
 * ================================================================ */

#define VMS_PAGE_SIZE       512
#define MAX_ZONES           64
#define EXTENT_SIZE         (64 * 1024)  /* 64 KB per extent */
#define LOOKASIDE_CLASSES   9            /* 8,16,32,64,128,256,512,1024,2048 */
#define MAX_LOOKASIDE_SIZE  2048
#define BLOCK_ALIGN         8            /* minimum alignment */

/* Lookaside bin sizes */
static const uint32_t bin_sizes[LOOKASIDE_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};

/* ================================================================
 * Internal structures
 * ================================================================ */

/* Header prepended to every allocation (8 bytes) */
struct alloc_hdr {
    uint32_t size;          /* usable size (not including header) */
    uint32_t zone_id;       /* owning zone (for validation) */
};

/* Free block in a lookaside list */
struct free_block {
    struct free_block *next;
};

/* An mmap'd memory extent belonging to a zone */
struct extent {
    struct extent   *next;
    char            *base;      /* mmap'd base address */
    uint32_t         size;      /* total mmap size */
    uint32_t         used;      /* bytes allocated from this extent */
};

/* A memory zone */
struct vm_zone {
    int              active;
    uint32_t         zone_id;
    pthread_mutex_t  lock;

    /* Zone name (from lib$create_vm_zone); empty for the default zone */
    char             name[64];

    /* Lookaside free lists for small allocations */
    struct free_block *lookaside[LOOKASIDE_CLASSES];

    /* Bump allocator state for current extent */
    char            *bump_ptr;
    uint32_t         bump_remain;

    /* Linked list of all extents (for bulk free) */
    struct extent   *extents;

    /* Statistics */
    uint64_t         total_alloc;
    uint64_t         total_free;
    uint64_t         get_vm_calls;   /* successful lib$get_vm calls */
    uint64_t         free_vm_calls;  /* successful lib$free_vm calls */
    uint64_t         expand_calls;   /* times a new extent was mmap'd */
};

/* ================================================================
 * Global state
 * ================================================================ */

static struct vm_zone zones[MAX_ZONES];
static pthread_mutex_t zone_table_lock = PTHREAD_MUTEX_INITIALIZER;
static int zones_initialized = 0;

/* ================================================================
 * Internal helpers
 * ================================================================ */

static void ensure_init(void)
{
    if (zones_initialized)
        return;

    pthread_mutex_lock(&zone_table_lock);
    if (!zones_initialized) {
        memset(zones, 0, sizeof(zones));

        /* Zone 0 is the default process zone, always active */
        zones[0].active = 1;
        zones[0].zone_id = 0;
        pthread_mutex_init(&zones[0].lock, NULL);

        zones_initialized = 1;
    }
    pthread_mutex_unlock(&zone_table_lock);
}

/* Map which lookaside bin a size falls into, or -1 for large */
static int size_to_bin(uint32_t size)
{
    for (int i = 0; i < LOOKASIDE_CLASSES; i++) {
        if (size <= bin_sizes[i])
            return i;
    }
    return -1;
}

/* Allocate a new extent via mmap */
static struct extent *extent_alloc(uint32_t min_size)
{
    uint32_t map_size = min_size;
    if (map_size < EXTENT_SIZE)
        map_size = EXTENT_SIZE;

    /* Round up to page boundary */
    uint32_t page_size = 4096;
    map_size = (map_size + page_size - 1) & ~(page_size - 1);

    void *p = mmap(NULL, map_size + sizeof(struct extent),
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;

    /* Place extent header at the start of the mapping */
    struct extent *ext = (struct extent *)p;
    ext->next = NULL;
    ext->base = (char *)p + sizeof(struct extent);
    /* Align base to 8 bytes */
    uintptr_t aligned = ((uintptr_t)ext->base + BLOCK_ALIGN - 1) & ~(uintptr_t)(BLOCK_ALIGN - 1);
    ext->base = (char *)aligned;
    ext->size = map_size + sizeof(struct extent);
    ext->used = (uint32_t)(ext->base - (char *)p);

    return ext;
}

/* Allocate from a zone's bump allocator, creating new extent if needed */
static void *zone_bump_alloc(struct vm_zone *zone, uint32_t total_size)
{
    /* Try current bump region */
    if (zone->bump_remain >= total_size) {
        void *p = zone->bump_ptr;
        zone->bump_ptr += total_size;
        zone->bump_remain -= total_size;
        return p;
    }

    /* Need a new extent */
    struct extent *ext = extent_alloc(total_size + 64);
    if (!ext)
        return NULL;

    /* Link into zone's extent list */
    ext->next = zone->extents;
    zone->extents = ext;
    zone->expand_calls++;

    /* Set up bump allocator on new extent */
    uint32_t usable = ext->size - ext->used;
    zone->bump_ptr = ext->base;
    zone->bump_remain = usable;

    void *p = zone->bump_ptr;
    zone->bump_ptr += total_size;
    zone->bump_remain -= total_size;

    return p;
}

/* Allocate a large block (> MAX_LOOKASIDE_SIZE) as its own extent */
static void *zone_large_alloc(struct vm_zone *zone, uint32_t total_size)
{
    struct extent *ext = extent_alloc(total_size);
    if (!ext)
        return NULL;

    ext->next = zone->extents;
    zone->extents = ext;
    zone->expand_calls++;

    return ext->base;
}

/* ================================================================
 * Public API
 * ================================================================ */

/*
 * lib$get_vm - Allocate virtual memory from a zone.
 *
 * If zone_id is NULL or points to 0, uses the default zone (zone 0).
 * Prepends an 8-byte alloc_hdr for tracking.
 */
uint32_t lib$get_vm(const uint32_t *num_bytes, void **base_adr, ...)
{
    va_list ap;
    const uint32_t *zone_id_ptr = NULL;

    if (!num_bytes || !base_adr)
        return SS$_BADPARAM;
    if (*num_bytes == 0)
        return SS$_BADPARAM;

    ensure_init();

    /* Extract optional zone_id */
    va_start(ap, base_adr);
    zone_id_ptr = va_arg(ap, const uint32_t *);
    va_end(ap);

    uint32_t zid = 0;
    if (zone_id_ptr && *zone_id_ptr != 0)
        zid = *zone_id_ptr;

    if (zid >= MAX_ZONES || !zones[zid].active)
        return LIB$_BADZONE;

    struct vm_zone *zone = &zones[zid];
    uint32_t user_size = *num_bytes;
    uint32_t total_size = user_size + sizeof(struct alloc_hdr);

    /* Align total size up */
    total_size = (total_size + BLOCK_ALIGN - 1) & ~(uint32_t)(BLOCK_ALIGN - 1);

    pthread_mutex_lock(&zone->lock);

    void *raw = NULL;
    int bin = size_to_bin(user_size);

    if (bin >= 0) {
        /* Small allocation: try lookaside list first */
        if (zone->lookaside[bin]) {
            struct free_block *fb = zone->lookaside[bin];
            zone->lookaside[bin] = fb->next;
            raw = (void *)fb;
        } else {
            /* Bump-allocate from extent with bin-sized total */
            uint32_t bin_total = bin_sizes[bin] + sizeof(struct alloc_hdr);
            bin_total = (bin_total + BLOCK_ALIGN - 1) & ~(uint32_t)(BLOCK_ALIGN - 1);
            raw = zone_bump_alloc(zone, bin_total);
            total_size = bin_total;
        }
    } else {
        /* Large allocation: own mmap region */
        raw = zone_large_alloc(zone, total_size);
    }

    if (!raw) {
        pthread_mutex_unlock(&zone->lock);
        return SS$_INSFMEM;
    }

    zone->total_alloc += user_size;
    zone->get_vm_calls++;
    pthread_mutex_unlock(&zone->lock);

    /* Write header */
    struct alloc_hdr *hdr = (struct alloc_hdr *)raw;
    hdr->size = (bin >= 0) ? bin_sizes[bin] : user_size;
    hdr->zone_id = zid;

    *base_adr = (char *)raw + sizeof(struct alloc_hdr);
    /* Zero the returned block.  VMS callers routinely create zones with
     * LIB$M_VM_GET_FILL0 (e.g. MMK's symbol/rule/dependency zones) and rely on
     * fresh allocations being zeroed — a struct whose pointer fields must read
     * as NULL until set.  Lookaside reuse returns dirty memory, so zero here to
     * honour the FILL0 contract.  (vms-ec70) */
    memset(*base_adr, 0, hdr->size);
    return SS$_NORMAL;
}

/*
 * lib$free_vm - Free virtual memory to a zone.
 *
 * Returns the block to the zone's lookaside list if small,
 * or marks it as freed for large blocks (actual munmap happens
 * only on zone deletion).
 */
uint32_t lib$free_vm(const uint32_t *num_bytes, void **base_adr, ...)
{
    va_list ap;
    const uint32_t *zone_id_ptr = NULL;

    (void)num_bytes;

    if (!base_adr || !*base_adr)
        return SS$_BADPARAM;

    ensure_init();

    va_start(ap, base_adr);
    zone_id_ptr = va_arg(ap, const uint32_t *);
    va_end(ap);

    /* Find the header */
    struct alloc_hdr *hdr = (struct alloc_hdr *)((char *)*base_adr - sizeof(struct alloc_hdr));
    uint32_t zid = hdr->zone_id;

    /* Validate zone_id if caller provided one */
    if (zone_id_ptr && *zone_id_ptr != 0) {
        if (*zone_id_ptr != zid)
            return LIB$_BADZONE;
    }

    if (zid >= MAX_ZONES || !zones[zid].active)
        return LIB$_BADZONE;

    struct vm_zone *zone = &zones[zid];

    pthread_mutex_lock(&zone->lock);

    int bin = size_to_bin(hdr->size);
    if (bin >= 0) {
        /* Return to lookaside list */
        struct free_block *fb = (struct free_block *)hdr;
        fb->next = zone->lookaside[bin];
        zone->lookaside[bin] = fb;
    }
    /* Large blocks: no individual free, reclaimed on zone delete.
     * The memory stays mapped until lib$delete_vm_zone. */

    zone->total_free += hdr->size;
    zone->free_vm_calls++;
    pthread_mutex_unlock(&zone->lock);

    *base_adr = NULL;
    return SS$_NORMAL;
}

/*
 * lib$create_vm_zone - Create a virtual memory zone.
 *
 * Allocates a zone slot and returns the zone identifier.
 * VMS supports algorithm selection and zone parameters; we use
 * quick-fit for all zones.
 */
uint32_t lib$create_vm_zone(uint32_t *zone_id, ...)
{
    if (!zone_id)
        return SS$_BADPARAM;

    ensure_init();

    /*
     * Extract the optional zone name. In the VMS argument order the
     * zone-name descriptor is the 11th argument overall, i.e. the 10th
     * variadic argument after zone-id:
     *   algorithm(1) algorithm-arg(2) flags(3) extend-size(4)
     *   initial-size(5) block-size(6) alignment(7) page-limit(8)
     *   smallest-block-size(9) zone-name(10) [get-page(11) free-page(12)]
     */
    const struct dsc$descriptor_s *name_dsc = NULL;
    {
        va_list ap;
        va_start(ap, zone_id);
        for (int i = 1; i <= 10; i++) {
            void *arg = va_arg(ap, void *);
            if (i == 10)
                name_dsc = (const struct dsc$descriptor_s *)arg;
        }
        va_end(ap);
    }

    pthread_mutex_lock(&zone_table_lock);

    /* Find free slot (skip zone 0 = default) */
    uint32_t zid;
    for (zid = 1; zid < MAX_ZONES; zid++) {
        if (!zones[zid].active)
            break;
    }

    if (zid >= MAX_ZONES) {
        pthread_mutex_unlock(&zone_table_lock);
        return SS$_INSFMEM;  /* No free zone slots */
    }

    memset(&zones[zid], 0, sizeof(struct vm_zone));
    zones[zid].active = 1;
    zones[zid].zone_id = zid;
    pthread_mutex_init(&zones[zid].lock, NULL);

    /* Record the zone name if one was supplied. */
    if (name_dsc && name_dsc->dsc$a_pointer && name_dsc->dsc$w_length > 0) {
        uint16_t n = name_dsc->dsc$w_length;
        if (n > sizeof(zones[zid].name) - 1)
            n = sizeof(zones[zid].name) - 1;
        memcpy(zones[zid].name, name_dsc->dsc$a_pointer, n);
        zones[zid].name[n] = '\0';
    }

    pthread_mutex_unlock(&zone_table_lock);

    *zone_id = zid;
    return SS$_NORMAL;
}

/*
 * lib$delete_vm_zone - Delete a zone and bulk-free all its memory.
 *
 * This is the killer feature of VMS zones: all memory allocated
 * from the zone is freed in one shot by munmap'ing all extents.
 * No need to track individual allocations.
 */
uint32_t lib$delete_vm_zone(const uint32_t *zone_id)
{
    if (!zone_id)
        return SS$_BADPARAM;

    uint32_t zid = *zone_id;

    if (zid == 0)
        return LIB$_BADZONE;  /* Cannot delete the default zone */

    ensure_init();

    pthread_mutex_lock(&zone_table_lock);

    if (zid >= MAX_ZONES || !zones[zid].active) {
        pthread_mutex_unlock(&zone_table_lock);
        return LIB$_BADZONE;
    }

    struct vm_zone *zone = &zones[zid];

    pthread_mutex_lock(&zone->lock);

    /* munmap all extents */
    struct extent *ext = zone->extents;
    while (ext) {
        struct extent *next = ext->next;
        munmap((void *)ext, ext->size);
        ext = next;
    }

    pthread_mutex_unlock(&zone->lock);
    pthread_mutex_destroy(&zone->lock);

    /* Mark slot as free */
    zone->active = 0;
    zone->extents = NULL;

    pthread_mutex_unlock(&zone_table_lock);

    return SS$_NORMAL;
}

/*
 * lib$reset_vm_zone - Bulk-free all memory in a zone but keep the zone ALIVE.
 *
 * Like lib$delete_vm_zone, all extents are munmap'd in one shot; unlike delete,
 * the zone slot stays active and is re-initialised empty so the caller can keep
 * allocating from it.  (VMS callers use this to recycle a scratch zone between
 * passes — e.g. MMK's pattern-substitution leaf zone.)  Added for vms-ec70.
 */
uint32_t lib$reset_vm_zone(const uint32_t *zone_id)
{
    if (!zone_id)
        return SS$_BADPARAM;

    uint32_t zid = *zone_id;

    ensure_init();

    pthread_mutex_lock(&zone_table_lock);

    if (zid >= MAX_ZONES || !zones[zid].active) {
        pthread_mutex_unlock(&zone_table_lock);
        return LIB$_BADZONE;
    }

    struct vm_zone *zone = &zones[zid];

    pthread_mutex_lock(&zone->lock);

    struct extent *ext = zone->extents;
    while (ext) {
        struct extent *next = ext->next;
        munmap((void *)ext, ext->size);
        ext = next;
    }
    zone->extents     = NULL;
    zone->bump_ptr    = NULL;
    zone->bump_remain = 0;
    for (int i = 0; i < LOOKASIDE_CLASSES; i++)
        zone->lookaside[i] = NULL;
    zone->total_alloc = 0;
    zone->total_free  = 0;

    pthread_mutex_unlock(&zone->lock);
    pthread_mutex_unlock(&zone_table_lock);

    return SS$_NORMAL;
}

/*
 * lib$get_vm_page - Allocate memory in VMS page units (512 bytes each).
 */
uint32_t lib$get_vm_page(const uint32_t *num_pages, void **base_adr)
{
    if (!num_pages || !base_adr)
        return SS$_BADPARAM;

    uint32_t bytes = *num_pages * VMS_PAGE_SIZE;
    /* lib$get_vm is variadic (optional zone_id); it always va_arg-reads one
     * pointer.  Forwarding without it makes that read walk past our argument
     * list and dereference garbage as a zone_id pointer.  On x86_64/aarch64 the
     * over-read slot happened to be benign; on Alpha's varargs ABI it is a live
     * stack value and the deref at "*zone_id_ptr" faults.  Pass an explicit NULL
     * (default zone) so the read is defined. */
    return lib$get_vm(&bytes, base_adr, (const uint32_t *)NULL);
}

/*
 * lib$free_vm_page - Free memory allocated by lib$get_vm_page.
 */
uint32_t lib$free_vm_page(const uint32_t *num_pages, void **base_adr)
{
    uint32_t bytes = num_pages ? *num_pages * VMS_PAGE_SIZE : 0;
    /* Same variadic contract as lib$get_vm_page above: pass the optional
     * zone_id explicitly (NULL = default zone) so lib$free_vm's va_arg read is
     * defined and does not fault on Alpha. */
    return lib$free_vm(&bytes, base_adr, (const uint32_t *)NULL);
}

/* ================================================================
 * Zone introspection / statistics
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$SHOW_VM,
 * LIB$STAT_VM, LIB$FIND_VM_ZONE, LIB$SHOW_VM_ZONE, LIB$VERIFY_VM_ZONE.
 * ================================================================ */

/*
 * lib$stat_vm - Return one statistic about the default zone.
 *
 * Documented statistic codes (LIB$K_VM_*):
 *   1 = number of calls to LIB$GET_VM
 *   2 = number of calls to LIB$FREE_VM
 *   3 = number of times memory was expanded (extents mmap'd)
 *   4 = number of bytes still allocated
 * The statistics describe the default (process) zone, zone 0.
 */
uint32_t lib$stat_vm(const int32_t *code, uint32_t *value)
{
    if (!code || !value)
        return SS$_BADPARAM;

    ensure_init();

    struct vm_zone *zone = &zones[0];
    pthread_mutex_lock(&zone->lock);

    uint32_t result;
    switch (*code) {
    case 1: result = (uint32_t)zone->get_vm_calls;  break;
    case 2: result = (uint32_t)zone->free_vm_calls; break;
    case 3: result = (uint32_t)zone->expand_calls;  break;
    case 4: result = (uint32_t)(zone->total_alloc - zone->total_free); break;
    default:
        pthread_mutex_unlock(&zone->lock);
        return LIB$_INVARG;
    }

    pthread_mutex_unlock(&zone->lock);
    *value = result;
    return SS$_NORMAL;
}

/*
 * lib$show_vm - Output summary statistics for the default zone.
 *
 * Writes a one-line summary of the default zone's allocation activity to
 * SYS$OUTPUT (stdout here). VMS accepts an optional code, output routine
 * and argument; when no output routine is supplied the summary goes to
 * SYS$OUTPUT.
 */
uint32_t lib$show_vm(void)
{
    ensure_init();

    struct vm_zone *zone = &zones[0];
    pthread_mutex_lock(&zone->lock);
    uint64_t gets = zone->get_vm_calls;
    uint64_t frees = zone->free_vm_calls;
    uint64_t expands = zone->expand_calls;
    pthread_mutex_unlock(&zone->lock);

    printf("LIB$GET_VM count = %llu, LIB$FREE_VM count = %llu, "
           "expansions = %llu\n",
           (unsigned long long)gets,
           (unsigned long long)frees,
           (unsigned long long)expands);
    fflush(stdout);

    return SS$_NORMAL;
}

/*
 * lib$find_vm_zone - Iterate over all active 32-bit zones.
 *
 * context must be initialized to 0 before the first call; it holds the
 * id of the next zone to examine. Each call returns the id of the next
 * active zone (including the default zone 0) through zone_id and advances
 * context. Returns LIB$_NOTFOU when no more zones exist.
 */
uint32_t lib$find_vm_zone(uint32_t *context, uint32_t *zone_id)
{
    if (!context || !zone_id)
        return SS$_BADPARAM;

    ensure_init();

    pthread_mutex_lock(&zone_table_lock);
    for (uint32_t zid = *context; zid < MAX_ZONES; zid++) {
        if (zones[zid].active) {
            *zone_id = zid;
            *context = zid + 1;
            pthread_mutex_unlock(&zone_table_lock);
            return SS$_NORMAL;
        }
    }
    pthread_mutex_unlock(&zone_table_lock);

    return LIB$_NOTFOU;
}

/*
 * lib$show_vm_zone - Format information about one zone and deliver it.
 *
 * Produces the standard default line
 *   'Zone ID = nnnnnn, Zone Name = "name"'
 * and passes it, as a string descriptor, to the caller's action routine
 * together with user_arg. When no action routine is supplied the line is
 * written to SYS$OUTPUT (stdout). detail selects the amount of output on
 * VMS; here the summary line is always produced.
 */
uint32_t lib$show_vm_zone(const uint32_t *zone_id,
                          const int32_t *detail,
                          uint32_t (*action_rtn)(struct dsc$descriptor_s *,
                                                 void *),
                          void *user_arg)
{
    (void)detail;

    if (!zone_id)
        return SS$_BADPARAM;

    uint32_t zid = *zone_id;
    if (zid >= MAX_ZONES || !zones[zid].active)
        return LIB$_BADZONE;

    char line[128];
    int len = snprintf(line, sizeof(line),
                       "Zone ID = %u, Zone Name = \"%s\"",
                       zid, zones[zid].name);
    if (len < 0)
        len = 0;
    if (len > (int)sizeof(line) - 1)
        len = (int)sizeof(line) - 1;

    struct dsc$descriptor_s line_d = {
        (uint16_t)len, DSC$K_DTYPE_T, DSC$K_CLASS_S, line
    };

    if (action_rtn)
        return action_rtn(&line_d, user_arg);

    printf("%.*s\n", len, line);
    fflush(stdout);
    return SS$_NORMAL;
}

/*
 * lib$verify_vm_zone - Verify the integrity of a zone.
 *
 * A full VMS implementation walks the zone's internal structures. Here we
 * validate that the zone id refers to an active zone and that its extent
 * chain is self-consistent (each extent's used count fits its size).
 */
uint32_t lib$verify_vm_zone(const uint32_t *zone_id)
{
    if (!zone_id)
        return SS$_BADPARAM;

    uint32_t zid = *zone_id;
    if (zid >= MAX_ZONES || !zones[zid].active)
        return LIB$_BADZONE;

    struct vm_zone *zone = &zones[zid];
    pthread_mutex_lock(&zone->lock);
    for (struct extent *e = zone->extents; e; e = e->next) {
        if (e->used > e->size) {
            pthread_mutex_unlock(&zone->lock);
            return LIB$_INVARG;
        }
    }
    pthread_mutex_unlock(&zone->lock);

    return SS$_NORMAL;
}
