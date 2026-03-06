/*
 * sys_lock.c - Lock Manager System Services ($ENQ, $DEQ)
 *
 * VMS distributed lock manager emulated using local lock files.
 * Lock resources are represented as files in /tmp/ovmx/locks/,
 * and POSIX flock()/fcntl() is used for actual mutual exclusion.
 *
 * Lock modes (VMS compatible):
 *   LCK$K_NLMODE (0) - Null lock (no access)
 *   LCK$K_CRMODE (1) - Concurrent Read
 *   LCK$K_CWMODE (2) - Concurrent Write
 *   LCK$K_PRMODE (3) - Protected Read
 *   LCK$K_PWMODE (4) - Protected Write
 *   LCK$K_EXMODE (5) - Exclusive
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <pthread.h>
#include <errno.h>
#include "starlet.h"

/* Lock modes */
#define LCK$K_NLMODE  0  /* Null */
#define LCK$K_CRMODE  1  /* Concurrent Read */
#define LCK$K_CWMODE  2  /* Concurrent Write */
#define LCK$K_PRMODE  3  /* Protected Read */
#define LCK$K_PWMODE  4  /* Protected Write */
#define LCK$K_EXMODE  5  /* Exclusive */

/* Lock Status Block */
struct lksb {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[16];  /* Lock value block */
};

#define MAX_LOCKS 256
#define LOCK_DIR "/tmp/ovmx/locks"

/* Internal lock table entry */
struct lock_entry {
    uint32_t  lkid;
    char      resnam[64];      /* Resource name */
    uint32_t  lkmode;          /* Granted lock mode */
    int       lock_fd;         /* File descriptor for lock file */
    struct lksb *lksb;         /* Pointer to caller's LKSB */
    int       in_use;
};

static struct lock_entry locks[MAX_LOCKS];
static uint32_t next_lkid = 1;
static pthread_mutex_t lock_mgr_mutex = PTHREAD_MUTEX_INITIALIZER;
static int lock_dir_created = 0;

/* Ensure the lock directory exists */
static void ensure_lock_dir(void) {
    if (lock_dir_created) return;
    mkdir("/tmp/ovmx", 0777);
    mkdir(LOCK_DIR, 0777);
    lock_dir_created = 1;
}

/*
 * sys$enqw - Enqueue lock request and wait (synchronous).
 *
 * Acquires a lock on the named resource at the requested mode.
 * Creates a lock file in /tmp/ovmx/locks/ and uses flock() to
 * acquire the actual OS-level lock.
 *
 * Modes NLMODE-CRMODE use shared locks; PRMODE-EXMODE use exclusive locks.
 *
 * Parameters:
 *   efn      - Event flag to set on completion
 *   lkmode   - Lock mode (LCK$K_xxx)
 *   lksb     - Lock status block
 *   flags    - Lock flags (ignored)
 *   resnam   - Resource name descriptor
 *   parid    - Parent lock ID (ignored)
 *   astadr   - AST completion routine (ignored for $ENQW)
 *   astprm   - AST parameter
 *   blkastadr- Blocking AST routine (ignored)
 *   acmode   - Access mode (ignored)
 *   rsdm_id  - Resource domain ID (ignored)
 */
uint32_t sys$enqw(uint32_t efn, uint32_t lkmode, void *lksb_ptr,
                  uint32_t flags, const struct dsc$descriptor_s *resnam,
                  uint32_t parid, void (*astadr)(uint32_t), uint32_t astprm,
                  void (*blkastadr)(uint32_t), uint32_t acmode,
                  uint32_t rsdm_id) {
    (void)efn; (void)flags; (void)parid; (void)astadr; (void)astprm;
    (void)blkastadr; (void)acmode; (void)rsdm_id;

    struct lksb *lksb = (struct lksb *)lksb_ptr;
    if (!lksb) return SS$_BADPARAM;

    char name[64] = "";
    if (resnam && resnam->dsc$a_pointer) {
        dsc$strncpy(name, resnam, sizeof(name));
    }

    ensure_lock_dir();

    pthread_mutex_lock(&lock_mgr_mutex);

    /* Find a free lock slot */
    int slot = -1;
    for (int i = 0; i < MAX_LOCKS; i++) {
        if (!locks[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&lock_mgr_mutex);
        lksb->lksb$w_status = (uint16_t)SS$_EXENQLM;
        return SS$_EXENQLM;
    }

    /* Reject resource names containing path separators or traversal */
    if (strchr(name, '/') || strstr(name, "..")) {
        pthread_mutex_unlock(&lock_mgr_mutex);
        lksb->lksb$w_status = (uint16_t)SS$_BADPARAM;
        return SS$_BADPARAM;
    }

    /* Create/open the lock file for this resource */
    char lockpath[256];
    snprintf(lockpath, sizeof(lockpath), "%s/%s.lck", LOCK_DIR, name);

    int lock_fd = open(lockpath, O_CREAT | O_RDWR, 0666);
    if (lock_fd < 0) {
        pthread_mutex_unlock(&lock_mgr_mutex);
        lksb->lksb$w_status = (uint16_t)SS$_BADPARAM;
        return SS$_BADPARAM;
    }

    /* Acquire the flock based on mode */
    int flock_op;
    if (lkmode <= LCK$K_CWMODE) {
        /* Null, CR, CW - use shared lock */
        flock_op = LOCK_SH;
    } else {
        /* PR, PW, EX - use exclusive lock */
        flock_op = LOCK_EX;
    }

    if (lkmode != LCK$K_NLMODE) {
        if (flock(lock_fd, flock_op) < 0) {
            close(lock_fd);
            pthread_mutex_unlock(&lock_mgr_mutex);
            lksb->lksb$w_status = (uint16_t)SS$_DEADLOCK;
            return SS$_DEADLOCK;
        }
    }

    /* Fill in the lock table entry */
    locks[slot].lkid = next_lkid++;
    strncpy(locks[slot].resnam, name, sizeof(locks[slot].resnam) - 1);
    locks[slot].resnam[sizeof(locks[slot].resnam) - 1] = '\0';
    locks[slot].lkmode = lkmode;
    locks[slot].lock_fd = lock_fd;
    locks[slot].lksb = lksb;
    locks[slot].in_use = 1;

    /* Set lock status block */
    lksb->lksb$w_status = (uint16_t)SS$_NORMAL;
    lksb->lksb$l_lkid = locks[slot].lkid;

    pthread_mutex_unlock(&lock_mgr_mutex);

    /* Set event flag */
    if (efn > 0) {
        sys$setef(efn);
    }

    return SS$_NORMAL;
}

/*
 * sys$enq - Enqueue lock request (asynchronous).
 *
 * Currently implemented as synchronous (same as $ENQW).
 */
uint32_t sys$enq(uint32_t efn, uint32_t lkmode, void *lksb,
                 uint32_t flags, const struct dsc$descriptor_s *resnam,
                 uint32_t parid, void (*astadr)(uint32_t), uint32_t astprm,
                 void (*blkastadr)(uint32_t), uint32_t acmode,
                 uint32_t rsdm_id) {
    return sys$enqw(efn, lkmode, lksb, flags, resnam, parid,
                    astadr, astprm, blkastadr, acmode, rsdm_id);
}

/*
 * sys$deq - Dequeue (release) a lock.
 *
 * Releases the lock identified by lkid, unlocking the file lock
 * and closing the lock file descriptor.
 *
 * Parameters:
 *   lkid   - Lock ID (from $ENQ lksb)
 *   valblk - Optional value block to store in the lock (16 bytes)
 *   acmode - Access mode (ignored)
 *   flags  - Dequeue flags (ignored)
 */
uint32_t sys$deq(uint32_t lkid, void *valblk, uint32_t acmode,
                 uint32_t flags) {
    (void)acmode; (void)flags;

    pthread_mutex_lock(&lock_mgr_mutex);

    for (int i = 0; i < MAX_LOCKS; i++) {
        if (locks[i].in_use && locks[i].lkid == lkid) {
            /* Copy value block if provided */
            if (valblk && locks[i].lksb) {
                memcpy(locks[i].lksb->lksb$b_valblk, valblk, 16);
            }

            /* Release the file lock and close */
            if (locks[i].lock_fd >= 0) {
                flock(locks[i].lock_fd, LOCK_UN);
                close(locks[i].lock_fd);
            }

            locks[i].in_use = 0;
            locks[i].lock_fd = -1;
            pthread_mutex_unlock(&lock_mgr_mutex);
            return SS$_NORMAL;
        }
    }

    pthread_mutex_unlock(&lock_mgr_mutex);
    return SS$_BADPARAM;
}
