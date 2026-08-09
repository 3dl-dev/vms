/*
 * lib_queue.c - LIB$ self-relative interlocked queue routines
 *
 * Implements:
 *
 *   LIB$INSQHI  - Insert entry at head of a self-relative queue
 *   LIB$INSQTI  - Insert entry at tail of a self-relative queue
 *   LIB$REMQHI  - Remove entry from head of a self-relative queue
 *   LIB$REMQTI  - Remove entry from tail of a self-relative queue
 *
 * A self-relative queue is a doubly linked circular list in which every
 * link is stored as a self-relative displacement: the value at a link
 * cell is (target_address - address_of_the_link_cell), so 0 means "points
 * to itself".  The queue header is a quadword holding two longword
 * displacements (forward link at offset 0, backward link at offset 4),
 * and each entry begins with the same two-longword link pair.  This is
 * the VAX self-relative queue format the LIB$INSQxI / LIB$REMQxI jackets
 * operate on, so the on-memory representation is byte-compatible with a
 * queue built or consumed by other VMS code.
 *
 * The VAX INSQHI/REMQHI instructions were "interlocked" (atomic across
 * processors); the retry_count argument controlled the secondary
 * interlock spin.  On OVMX these operate on process-local memory, so the
 * displacement bookkeeping is what matters and retry_count is accepted
 * for source compatibility.  Because the header is a single quadword,
 * the displacements are longwords (32-bit) exactly as on the VAX.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$INSQHI, LIB$INSQTI,
 *            LIB$REMQHI, LIB$REMQTI; OpenVMS VAX Architecture Reference
 *            Manual (self-relative queue format and instruction status).
 */

#include <stdint.h>
#include "ssdef.h"
#include "libdef.h"
#include "lib$routines.h"

/* Forward link (offset 0) and backward link (offset 4) longword cells. */
static inline int32_t *q_fl(void *loc) { return (int32_t *)loc; }
static inline int32_t *q_bl(void *loc) { return (int32_t *)((char *)loc + 4); }

/* Follow a self-relative displacement to the absolute address. */
static inline void *q_follow(void *loc, int32_t disp)
{
    return (char *)loc + disp;
}

/* Compute the self-relative displacement from 'from' to 'to'. */
static inline int32_t q_disp(void *from, void *to)
{
    return (int32_t)((char *)to - (char *)from);
}

/*
 * lib$insqhi - Insert 'entry' at the head of the queue (after the header).
 */
uint32_t lib$insqhi(void *entry, void *header, const uint32_t *retry_count)
{
    (void)retry_count;
    if (!entry || !header)
        return SS$_BADPARAM;

    void *old_first = q_follow(header, *q_fl(header));

    *q_fl(entry) = q_disp(entry, old_first);
    *q_bl(entry) = q_disp(entry, header);
    *q_bl(old_first) = q_disp(old_first, entry);
    *q_fl(header) = q_disp(header, entry);

    return (old_first == header) ? LIB$_ONEENTQUE : SS$_NORMAL;
}

/*
 * lib$insqti - Insert 'entry' at the tail of the queue (before the header).
 */
uint32_t lib$insqti(void *entry, void *header, const uint32_t *retry_count)
{
    (void)retry_count;
    if (!entry || !header)
        return SS$_BADPARAM;

    void *old_last = q_follow(header, *q_bl(header));

    *q_fl(entry) = q_disp(entry, header);
    *q_bl(entry) = q_disp(entry, old_last);
    *q_fl(old_last) = q_disp(old_last, entry);
    *q_bl(header) = q_disp(header, entry);

    return (old_last == header) ? LIB$_ONEENTQUE : SS$_NORMAL;
}

/*
 * lib$remqhi - Remove the entry at the head of the queue.  *addr receives
 * the address of the removed entry.
 */
uint32_t lib$remqhi(void *header, void *addr, const uint32_t *retry_count)
{
    (void)retry_count;
    if (!header || !addr)
        return SS$_BADPARAM;

    if (*q_fl(header) == 0)
        return LIB$_QUEWASEMP;

    void *first  = q_follow(header, *q_fl(header));
    void *second = q_follow(first, *q_fl(first));

    *q_fl(header) = q_disp(header, second);
    *q_bl(second) = q_disp(second, header);

    *(void **)addr = first;
    return (second == header) ? LIB$_ONEENTQUE : SS$_NORMAL;
}

/*
 * lib$remqti - Remove the entry at the tail of the queue.  *addr receives
 * the address of the removed entry.
 */
uint32_t lib$remqti(void *header, void *addr, const uint32_t *retry_count)
{
    (void)retry_count;
    if (!header || !addr)
        return SS$_BADPARAM;

    if (*q_bl(header) == 0)
        return LIB$_QUEWASEMP;

    void *last = q_follow(header, *q_bl(header));
    void *prev = q_follow(last, *q_bl(last));

    *q_bl(header) = q_disp(header, prev);
    *q_fl(prev) = q_disp(prev, header);

    *(void **)addr = last;
    return (prev == header) ? LIB$_ONEENTQUE : SS$_NORMAL;
}
