/*
 * ovmx_decc_vm.c - the DEC C RTL heap's virtual memory, from the executive
 * (rd vms-bc53).
 *
 * The C RTL's malloc/free/realloc (musl mallocng in DECC$SHR) obtain and give
 * back address space ONLY through these entry points, which ask the executive's
 * $EXPREG / $DELTVA service (KIF_SVC_VA -> src/kernel/vms_va.c) -- the same
 * service SYS$EXPREG/SYS$DELTVA call. The executive maps the demand-zero pages
 * at the end of the process's P0 region and charges them to its PGFLQUOTA, so
 * the heap's address space, its quota and its exhaustion status are VMS state:
 * a request past the quota comes back SS$_EXQUOTA, and malloc returns NULL with
 * errno ENOMEM. Nothing here reaches Linux brk/mmap/munmap; with no executive
 * every call fails SS$_NOSUCHDEV and the heap cannot grow -- there is no
 * fallback (INV-6). The musl side is the mallocng glue overlay
 * tools/cross-alpha-vms/musl-arch/src/malloc/mallocng/glue.h.
 *
 * On VMS the DEC C RTL gets its heap from LIB$GET_VM / $EXPREG through the
 * system service vector; on OVMX/Alpha DECC$SHR reaches the executive through
 * libvmssys's transport seam (kif_calls.h), exactly as its vfork does
 * (ovmx_decc_vfork.c) -- the bootstrap DECC$SHR has no LIBVMS$SHR to import
 * SYS$EXPREG from.
 *
 * One executive handle per process, opened on first use and reused (a heap
 * that opened /dev/vms per allocation would spend two host calls on the
 * transport for every group). The executive identifies the caller by its task,
 * not by the handle, so a handle inherited across fork names the child.
 * mallocng calls these under its own lock, so the lazy open is not raced.
 */
#include <stdint.h>
#include <string.h>

#include "kif_calls.h"                 /* the executive, through the transport seam */

#define OVMX_SS_NOSUCHDEV 0x908u        /* SS$_NOSUCHDEV */
#define OVMX_SS_PAGOWNVIO 0x1ECu        /* SS$_PAGOWNVIO (492) */

static int vm_h = -1;

static unsigned int vm_call(struct vms_va_args *a)
{
    if (vm_h < 0) {
        vm_h = kif_xport_dev_open();
        if (vm_h < 0)
            return OVMX_SS_NOSUCHDEV;
    }
    a->status = OVMX_SS_NOSUCHDEV;     /* unless the executive answers */
    if (kif_call(vm_h, KIF_SVC_VA, a) < 0)
        return OVMX_SS_NOSUCHDEV;
    return a->status;
}

/*
 * THE HEAP REUSES THE ADDRESS SPACE IT GAVE BACK (rd vms-b85). $EXPREG only
 * ever appends at the end of P0, and $DELTVA of a range below the end leaves a
 * hole that $EXPREG never fills -- exactly as on VMS. A heap that gives a large
 * block back with $DELTVA while something above it is still live would walk up
 * P0 one block at a time and hit SS$_VASFULL at 1 GB with almost nothing in use
 * (GCC's cc1plus on gimple-match-*.cc did: "out of memory allocating 989320
 * bytes"). A VMS program that deletes pages and wants the addresses again
 * re-creates them with $CRETVA at the address it owns; the DEC C heap does the
 * same: it remembers the ranges it deleted (holes[], sorted, coalesced) and
 * fills a request from the first hole that fits with $CRETVA -- demand-zero
 * pages charged to PGFLQUOTA like any other. $CRETVA here is refused, never
 * forced, if anything else has been mapped into the hole since
 * (SS$_PAGOWNVIO): the hole is dropped and the next one, or $EXPREG, is tried.
 *
 * Every hole lies below the region's end (frep0va): $EXPREG only allocates at
 * or above it, and the end only moves down in a $DELTVA, whose reply carries
 * the new end -- the list is trimmed to it right there. So no hole can name
 * pages $EXPREG has since handed out again.
 */
#define VM_HOLES 256

static struct { unsigned long long s, e; } holes[VM_HOLES];
static int nholes;

static void hole_cut(int i)
{
    memmove(&holes[i], &holes[i + 1], (size_t)(nholes - i - 1) * sizeof holes[0]);
    nholes--;
}

/* Record [s, e) as deleted, merged with its neighbours; then drop what lies
 * at or above the region's end `top`. A full table forgets the range (the
 * addresses are lost to this heap, as they all were before). */
static void hole_add(unsigned long long s, unsigned long long e,
                     unsigned long long top)
{
    int i, j;

    if (e > top)
        e = top;
    if (s < e) {
        for (i = 0; i < nholes && holes[i].s < s; i++)
            ;
        if (i > 0 && holes[i - 1].e >= s) {         /* joins the one below */
            i--;
            if (e > holes[i].e)
                holes[i].e = e;
        } else if (nholes < VM_HOLES) {
            memmove(&holes[i + 1], &holes[i], (size_t)(nholes - i) * sizeof holes[0]);
            holes[i].s = s;
            holes[i].e = e;
            nholes++;
        } else {
            i = -1;
        }
        if (i >= 0)                                 /* swallows those above */
            for (j = i + 1; j < nholes && holes[j].s <= holes[i].e; ) {
                if (holes[j].e > holes[i].e)
                    holes[i].e = holes[j].e;
                hole_cut(j);
            }
    }
    while (nholes > 0 && holes[nholes - 1].s >= top)
        nholes--;
    if (nholes > 0 && holes[nholes - 1].e > top)
        holes[nholes - 1].e = top;
}

/* $CRETVA `len` bytes into the first hole that holds them; 0 if none did. */
static int hole_take(unsigned long long len, unsigned long long *addr)
{
    struct vms_va_args a;
    unsigned int st;
    int i;

    for (i = 0; i < nholes; ) {
        if (holes[i].e - holes[i].s < len) {
            i++;
            continue;
        }
        memset(&a, 0, sizeof a);
        a.op = VMS_VA_CRETVA;
        a.base = holes[i].s;
        a.length = len;
        st = vm_call(&a);
        if (!(st & 1)) {
            if (st == OVMX_SS_PAGOWNVIO) {          /* no longer ours to fill */
                hole_cut(i);
                continue;
            }
            return 0;                               /* EXQUOTA etc.: $EXPREG says the same */
        }
        *addr = a.base;
        if (a.base + a.length >= holes[i].e)
            hole_cut(i);
        else
            holes[i].s = a.base + a.length;
        return 1;
    }
    return 0;
}

/*
 * ovmx$vm_expreg - `len` bytes of new P0 pages: a hole this heap deleted
 * ($CRETVA), else $EXPREG at the end of P0. On success *addr is the first byte
 * (the executive rounds len up to its page size). Returns the VMS status:
 * SS$_EXQUOTA past PGFLQUOTA, SS$_VASFULL when P0 is full, SS$_NOSUCHDEV with
 * no executive.
 */
unsigned int ovmx$vm_expreg(unsigned long long len, unsigned long long *addr)
{
    struct vms_va_args a;
    unsigned int st;

    if (len && hole_take(len, addr))
        return 1;                                   /* SS$_NORMAL */
    memset(&a, 0, sizeof a);
    a.op = VMS_VA_EXPREG;
    a.region = VMS_VA_REGION_P0;
    a.length = len;
    st = vm_call(&a);
    if (st & 1)
        *addr = a.base;
    return st;
}

/*
 * ovmx$vm_deltva - $DELTVA [addr, addr+len): the executive deletes the pages
 * it created there and returns their charge; the range becomes a hole the heap
 * may re-create.
 */
unsigned int ovmx$vm_deltva(unsigned long long addr, unsigned long long len)
{
    struct vms_va_args a;
    unsigned int st;

    memset(&a, 0, sizeof a);
    a.op = VMS_VA_DELTVA;
    a.base = addr;
    a.length = len;
    st = vm_call(&a);
    if ((st & 1) && a.length)
        hole_add(a.base, a.base + a.length, a.frep0va);
    return st;
}
