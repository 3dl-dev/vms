/*
 * sys_uring.c - io_uring initialization and completion processing for VMS QIO
 *
 * Provides per-process io_uring instance for async I/O:
 *   - vms_uring_init(): setup io_uring ring (256 entries)
 *   - vms_uring_cleanup(): tear down ring
 *   - vms_uring_submit_rw(): submit a read/write SQE
 *   - vms_uring_process_completions(): process CQEs, fill IOSBs, set EFs, queue ASTs
 *   - vms_uring_wait_completion(): blocking wait for at least one completion
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <linux/io_uring.h>
#include <stdatomic.h>
#include <pthread.h>
#include "starlet.h"
#include "vms/pcb.h"

/* io_uring syscall wrappers (not always available in glibc) */
static int io_uring_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags) {
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, NULL, 0);
}

/* Ring buffer pointer structures */
struct uring_sq {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    uint32_t *flags;
    uint32_t *array;
};

struct uring_cq {
    uint32_t *head;
    uint32_t *tail;
    uint32_t *ring_mask;
    uint32_t *ring_entries;
    struct io_uring_cqe *cqes;
};

/* Per-SQE completion context */
struct qio_completion {
    void    *iosb;              /* IOSB to fill */
    uint32_t efn;               /* event flag to set */
    void   (*astadr)(uint32_t); /* AST completion routine */
    uint32_t astprm;            /* AST parameter */
    int      is_read;           /* 1=read, 0=write */
    int      in_use;            /* 1=slot is active */
};

#define URING_ENTRIES 256
#define MAX_INFLIGHT  256

/* Global (per-process) completion context table */
static struct qio_completion completions[MAX_INFLIGHT];
static pthread_mutex_t comp_lock = PTHREAD_MUTEX_INITIALIZER;

static struct uring_sq sq_ring;
static struct uring_cq cq_ring;
static struct io_uring_sqe *sqes;

/*
 * vms_uring_init - Initialize io_uring for the current process
 */
static int uring_initialized = 0;

int vms_uring_init(void) {
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return -1;
    if (pcb->uring_fd >= 0) return 0; /* already initialized */
    if (uring_initialized) return 0;  /* guard: don't re-init after cleanup */

    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    int fd = io_uring_setup(URING_ENTRIES, &params);
    if (fd < 0) return -1;

    /* mmap the SQ ring */
    size_t sq_ring_sz = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
    void *sq_ptr = mmap(NULL, sq_ring_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (sq_ptr == MAP_FAILED) { close(fd); return -1; }

    /* mmap the CQ ring */
    size_t cq_ring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    void *cq_ptr = mmap(NULL, cq_ring_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    if (cq_ptr == MAP_FAILED) {
        munmap(sq_ptr, sq_ring_sz);
        close(fd);
        return -1;
    }

    /* mmap the SQE array */
    size_t sqe_sz = params.sq_entries * sizeof(struct io_uring_sqe);
    void *sqe_ptr = mmap(NULL, sqe_sz, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (sqe_ptr == MAP_FAILED) {
        munmap(cq_ptr, cq_ring_sz);
        munmap(sq_ptr, sq_ring_sz);
        close(fd);
        return -1;
    }

    /* Set up SQ ring pointers */
    sq_ring.head = (uint32_t *)((char *)sq_ptr + params.sq_off.head);
    sq_ring.tail = (uint32_t *)((char *)sq_ptr + params.sq_off.tail);
    sq_ring.ring_mask = (uint32_t *)((char *)sq_ptr + params.sq_off.ring_mask);
    sq_ring.ring_entries = (uint32_t *)((char *)sq_ptr + params.sq_off.ring_entries);
    sq_ring.flags = (uint32_t *)((char *)sq_ptr + params.sq_off.flags);
    sq_ring.array = (uint32_t *)((char *)sq_ptr + params.sq_off.array);

    /* Set up CQ ring pointers */
    cq_ring.head = (uint32_t *)((char *)cq_ptr + params.cq_off.head);
    cq_ring.tail = (uint32_t *)((char *)cq_ptr + params.cq_off.tail);
    cq_ring.ring_mask = (uint32_t *)((char *)cq_ptr + params.cq_off.ring_mask);
    cq_ring.ring_entries = (uint32_t *)((char *)cq_ptr + params.cq_off.ring_entries);
    cq_ring.cqes = (struct io_uring_cqe *)((char *)cq_ptr + params.cq_off.cqes);

    sqes = (struct io_uring_sqe *)sqe_ptr;

    /* Store in PCB */
    pcb->uring_fd = fd;
    pcb->uring_sq = sq_ptr;
    pcb->uring_cq = cq_ptr;
    pcb->uring_sqes = sqe_ptr;
    pcb->uring_sq_size = (uint32_t)sq_ring_sz;
    pcb->uring_cq_size = (uint32_t)cq_ring_sz;

    memset(completions, 0, sizeof(completions));
    uring_initialized = 1;

    return 0;
}

/*
 * Allocate a completion slot
 */
static int alloc_completion(void *iosb, uint32_t efn,
                            void (*astadr)(uint32_t), uint32_t astprm,
                            int is_read) {
    pthread_mutex_lock(&comp_lock);
    for (int i = 0; i < MAX_INFLIGHT; i++) {
        if (!completions[i].in_use) {
            completions[i].iosb = iosb;
            completions[i].efn = efn;
            completions[i].astadr = astadr;
            completions[i].astprm = astprm;
            completions[i].is_read = is_read;
            completions[i].in_use = 1;
            pthread_mutex_unlock(&comp_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&comp_lock);
    return -1;
}

/*
 * Submit a read or write SQE
 */
int vms_uring_submit_rw(int fd, void *buf, uint32_t len, uint64_t offset,
                         int is_read, void *iosb, uint32_t efn,
                         void (*astadr)(uint32_t), uint32_t astprm) {
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb || pcb->uring_fd < 0) return -1;

    int comp_idx = alloc_completion(iosb, efn, astadr, astprm, is_read);
    if (comp_idx < 0) return -1;

    /* Get next SQE slot */
    uint32_t tail = *sq_ring.tail;
    uint32_t mask = *sq_ring.ring_mask;
    uint32_t idx = tail & mask;

    struct io_uring_sqe *sqe = &sqes[idx];
    memset(sqe, 0, sizeof(*sqe));

    sqe->opcode = is_read ? IORING_OP_READ : IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (uint64_t)(uintptr_t)buf;
    sqe->len = len;
    sqe->off = offset;
    sqe->user_data = (uint64_t)comp_idx;

    sq_ring.array[idx] = idx;

    /* Memory barrier before updating tail */
    __atomic_store_n(sq_ring.tail, tail + 1, __ATOMIC_RELEASE);

    /* Submit to kernel */
    io_uring_enter(pcb->uring_fd, 1, 0, 0);

    return 0;
}

/*
 * Process completions (poll CQ ring)
 * Returns number of completions processed
 */
int vms_uring_process_completions(void) {
    int processed = 0;
    uint32_t head = __atomic_load_n(cq_ring.head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(cq_ring.tail, __ATOMIC_ACQUIRE);
    uint32_t mask = *cq_ring.ring_mask;

    while (head != tail) {
        struct io_uring_cqe *cqe = &cq_ring.cqes[head & mask];
        int comp_idx = (int)cqe->user_data;

        if (comp_idx >= 0 && comp_idx < MAX_INFLIGHT) {
            struct qio_completion *comp = &completions[comp_idx];

            /* Fill IOSB */
            if (comp->iosb) {
                struct _iosb *iosb = (struct _iosb *)comp->iosb;
                if (cqe->res < 0) {
                    iosb->iosb$w_status = (uint16_t)SS$_ABORT;
                    iosb->iosb$w_bcnt = 0;
                } else if (cqe->res == 0 && comp->is_read) {
                    iosb->iosb$w_status = (uint16_t)SS$_ENDOFFILE;
                    iosb->iosb$w_bcnt = 0;
                } else {
                    iosb->iosb$w_status = (uint16_t)SS$_NORMAL;
                    /* Clamp to 16-bit; full count in dev_depend */
                    iosb->iosb$w_bcnt = (cqe->res > 65535) ? 65535 : (uint16_t)cqe->res;
                }
                iosb->iosb$l_dev_depend = (cqe->res > 0) ? (uint32_t)cqe->res : 0;
            }

            /* Set event flag */
            if (comp->efn != 0) {
                sys$setef(comp->efn);
            }

            /* Call AST completion routine */
            if (comp->astadr) {
                comp->astadr(comp->astprm);
            }

            /* Free completion slot */
            pthread_mutex_lock(&comp_lock);
            memset(comp, 0, sizeof(*comp));
            pthread_mutex_unlock(&comp_lock);
        }

        head++;
        processed++;
    }

    __atomic_store_n(cq_ring.head, head, __ATOMIC_RELEASE);
    return processed;
}

/*
 * Wait for at least one completion (blocking)
 * Used by sys$qiow
 */
int vms_uring_wait_completion(void) {
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb || pcb->uring_fd < 0) return -1;

    /* Wait for at least 1 completion */
    io_uring_enter(pcb->uring_fd, 0, 1, IORING_ENTER_GETEVENTS);
    return vms_uring_process_completions();
}

/*
 * vms_uring_cleanup - Tear down io_uring
 */
void vms_uring_cleanup(void) {
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb || pcb->uring_fd < 0) return;

    if (pcb->uring_sqes)
        munmap(pcb->uring_sqes, URING_ENTRIES * sizeof(struct io_uring_sqe));
    if (pcb->uring_cq)
        munmap(pcb->uring_cq, pcb->uring_cq_size);
    if (pcb->uring_sq)
        munmap(pcb->uring_sq, pcb->uring_sq_size);

    close(pcb->uring_fd);
    pcb->uring_fd = -1;
    pcb->uring_sq = NULL;
    pcb->uring_cq = NULL;
    pcb->uring_sqes = NULL;
}
