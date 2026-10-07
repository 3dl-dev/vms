/* vms-122 fixture: the musl mmap.c shape -- a weak no-op (__vm_wait) at offset 0
 * of $CODE$, the real routine after it, and a weak alias exporting the routine
 * under its DEC C name. A strong __vm_wait elsewhere overrides the weak one. */
#define weak_alias(old, new) extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
static void dummy(void) { }
weak_alias(dummy, __vm_wait);
long __impl(long a, long b) { __vm_wait(); return a * 3 + b; }
weak_alias(__impl, mmap);
