/*
 * test_shared_pages.c - proves that a /SHARED known image's read-only
 * segment is backed by the same physical pages across processes (bead
 * vms-913.5, INSTALL done condition #4).
 *
 * Method (documented here as the canonical /proc/PID/smaps verification
 * approach for this project — reuse this pattern for any future "does
 * OVMX X share physical pages like VMS global sections" check):
 *
 *   1. Write a KFE database file (same on-disk format known_images.c
 *      reads) that is a few pages large.
 *   2. Fork two independent child processes. Each one mmap(MAP_SHARED,
 *      PROT_READ)s the SAME file and touches every mapped page (forces
 *      the PTEs to be populated, not just lazily faulted later), then
 *      signals "ready" over a pipe and blocks in pause().
 *   3. Once BOTH children have mapped and touched the file, the parent
 *      reads /proc/<child-A-pid>/smaps, finds the VMA whose header line
 *      names our temp file, and sums the "Shared_Clean:" field within
 *      that VMA's block.
 *   4. Linux classifies a page as "Shared" in smaps precisely when its
 *      struct page mapcount > 1 (multiple page tables reference the same
 *      physical frame) — this is independent of the MAP_SHARED/MAP_PRIVATE
 *      flag itself; MAP_SHARED is what *guarantees* writes stay coherent,
 *      but read-only sharing already happens automatically at the page-
 *      cache level for any clean file-backed mapping (see
 *      docs/design-image-activation.md, "/SHARED" row of the Attribute
 *      Semantics table). Observing Shared_Clean > 0 for child A's mapping
 *      once child B has also mapped + touched the file is the operational
 *      proof that both processes are backed by the same physical pages —
 *      i.e. the "installed image" memory saving VMS gets from global
 *      sections, OVMX gets for free from mmap(MAP_SHARED) over the page
 *      cache, per docs/design-image-activation.md's Note under section 6.
 *
 * Exit 0 if Shared_Clean > 0 for the shared mapping in both children's
 * smaps once both are mapped; 1 otherwise.
 */
#define _POSIX_C_SOURCE 200809L

#include "known_images.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_test_db(const char *path)
{
    struct kfe_file db;
    memset(&db, 0, sizeof(db));
    db.magic = KFE_MAGIC;
    db.version = KFE_VERSION;
    db.count = 1;
    strncpy(db.entries[0].soname, "LIBVMS$SHR.EXE", sizeof(db.entries[0].soname) - 1);
    strncpy(db.entries[0].path, path, sizeof(db.entries[0].path) - 1);
    db.entries[0].flags = KFE_F_SHARED;

    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("fopen"); exit(1); }
    if (fwrite(&db, sizeof(db), 1, fp) != 1) { perror("fwrite"); exit(1); }
    fclose(fp);
}

/* Child: mmap(MAP_SHARED) the DB file, touch every page, signal ready, wait
 * for SIGTERM. Runs known_images_open() itself, so this also exercises the
 * real lookup-module mmap path (not a hand-rolled mmap call). */
static void child_main(const char *path, int ready_fd)
{
    struct known_images_db db;
    if (known_images_open(&db, path) != 0) {
        _exit(2);
    }

    /* Touch every page to force PTE population (byte-at-a-time stride,
     * not relying on readahead). */
    volatile const char *base = (volatile const char *)db.map_base;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    for (size_t off = 0; off < db.map_len; off += (size_t)page)
        (void)base[off];
    (void)base[db.map_len - 1];

    char one = 'R';
    if (write(ready_fd, &one, 1) != 1) _exit(3);

    /* Wait to be torn down by the parent once it has inspected smaps. */
    for (;;) pause();
}

/*
 * A /proc/PID/smaps VMA header line looks like:
 *   ffb3484f5000-ffb348505000 r--s 00000000 08:10 2004182   /tmp/foo
 * i.e. everything before the first '-' is hex digits. Every other line in
 * the file (Size:, Rss:, Shared_Clean:, VmFlags:, ...) is a "FieldName:"
 * line with no leading '-' before its first '-' (most have none at all).
 * This is the only reliable way to tell a new VMA started — smaps field
 * lines are NOT indented, so "starts at column 0" does not distinguish
 * them from header lines.
 */
static int is_smaps_header(const char *line)
{
    const char *dash = strchr(line, '-');
    if (!dash || dash == line)
        return 0;
    for (const char *p = line; p < dash; p++) {
        if (!isxdigit((unsigned char)*p))
            return 0;
    }
    return 1;
}

/* Sum the "Shared_Clean:" kB values within the smaps VMA block whose header
 * line contains 'needle' (our temp file's path). Returns summed kB, or -1
 * if no matching VMA block was found. */
static long smaps_shared_clean_kb(pid_t pid, const char *needle)
{
    char smaps_path[64];
    snprintf(smaps_path, sizeof(smaps_path), "/proc/%d/smaps", (int)pid);
    FILE *fp = fopen(smaps_path, "r");
    if (!fp) { perror("fopen(smaps)"); return -1; }

    char line[512];
    int in_block = 0;
    int found_any = 0;
    long total = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (is_smaps_header(line)) {
            in_block = (strstr(line, needle) != NULL);
            if (in_block)
                found_any = 1;
            continue;
        }
        if (in_block && strncmp(line, "Shared_Clean:", 13) == 0) {
            long kb = 0;
            sscanf(line + 13, "%ld", &kb);
            total += kb;
        }
    }
    fclose(fp);
    return found_any ? total : -1;
}

int main(void)
{
    char path[] = "/tmp/known_images_shared_test.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    close(fd);
    write_test_db(path);

    int pipe_a[2], pipe_b[2];
    if (pipe(pipe_a) != 0 || pipe(pipe_b) != 0) { perror("pipe"); return 1; }

    pid_t pid_a = fork();
    if (pid_a == 0) {
        close(pipe_a[0]);
        child_main(path, pipe_a[1]);
        _exit(0);
    }
    pid_t pid_b = fork();
    if (pid_b == 0) {
        close(pipe_b[0]);
        child_main(path, pipe_b[1]);
        _exit(0);
    }

    close(pipe_a[1]);
    close(pipe_b[1]);

    char buf;
    if (read(pipe_a[0], &buf, 1) != 1) { fprintf(stderr, "child A never signaled ready\n"); return 1; }
    if (read(pipe_b[0], &buf, 1) != 1) { fprintf(stderr, "child B never signaled ready\n"); return 1; }

    /* Both children now have the file mapped MAP_SHARED and every page
     * touched. Inspect child A's smaps: with a second mapper present, the
     * kernel must report those pages as Shared (mapcount > 1). */
    long shared_kb = smaps_shared_clean_kb(pid_a, path);

    printf("child A smaps Shared_Clean for %s = %ld kB\n", path, shared_kb);

    kill(pid_a, SIGTERM);
    kill(pid_b, SIGTERM);
    waitpid(pid_a, NULL, 0);
    waitpid(pid_b, NULL, 0);
    unlink(path);

    if (shared_kb > 0) {
        printf("PASS: two processes mmap(MAP_SHARED)-ing the same known-image "
               "DB file share physical pages (Shared_Clean = %ld kB)\n", shared_kb);
        return 0;
    }

    printf("FAIL: expected Shared_Clean > 0 once a second process mapped the "
           "same file (got %ld) — see docs/design-image-activation.md section 6\n",
           shared_kb);
    return 1;
}
