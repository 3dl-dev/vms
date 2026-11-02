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
 * ovmx$vm_expreg - $EXPREG `len` bytes onto the end of P0. On success *addr is
 * the first byte (the executive rounds len up to its page size). Returns the
 * VMS status: SS$_EXQUOTA past PGFLQUOTA, SS$_VASFULL when P0 is full,
 * SS$_NOSUCHDEV with no executive.
 */
unsigned int ovmx$vm_expreg(unsigned long long len, unsigned long long *addr)
{
    struct vms_va_args a;
    unsigned int st;

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
 * it created there and returns their charge.
 */
unsigned int ovmx$vm_deltva(unsigned long long addr, unsigned long long len)
{
    struct vms_va_args a;

    memset(&a, 0, sizeof a);
    a.op = VMS_VA_DELTVA;
    a.base = addr;
    a.length = len;
    return vm_call(&a);
}
