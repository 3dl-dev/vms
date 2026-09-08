/*
 * emutls_vms.c (vms-a7a) — a minimal emulated-TLS runtime for the OVMX
 * alpha-dec-vms build.
 *
 * WHY THIS FILE EXISTS. The alpha-dec-vms GCC port lowers every `__thread`
 * access to a call to __emutls_get_address(&__emutls_v.NAME) (emulated TLS is
 * the OpenVMS-target default; `-fno-emulated-tls` is not even a recognized
 * option on this cc1). But the toolchain's libgcc.a DELIBERATELY EXCLUDES the
 * emutls runtime objects (tools/cross-alpha-vms/build-toolchain.sh:224 — the
 * author assumed only musl, which manages TLS itself via the thread pointer and
 * never emits an emutls call, would be built). OVMX's OWN C code DOES use
 * `__thread` (vms_kif.c, vms_pcb.c, libvms rtl/lib_signal.c, ...), so cross-
 * compiling those TUs into the VMS-native producer shareables leaves
 * __emutls_get_address undefined and the STRICT link fails %LINK-F-UNDEF. This
 * file provides that one runtime entry point, compiled into LIBVMSSYS$SHR (the
 * base producer every other shareable --use's) and exported as a universal, so
 * the whole producer graph's __thread references bind.
 *
 * THE emutls ABI (as the alpha-dec-vms cc1 emits it — verified from generated
 * assembly, vms-a7a). For `__thread T x;` the compiler emits a control object
 *   __emutls_v.x:  .quad size ; .quad align ; .quad loc(=0) ; .quad &templ
 * and replaces &x with __emutls_get_address(&__emutls_v.x). `templ` is the
 * initializer image (NULL => zero-init). `loc` is scratch owned entirely by the
 * runtime (the compiler zero-initializes it and never reads it), so this
 * implementation is free to use it as a direct pointer to the object's storage.
 *
 * SINGLE-THREADED storage (the honest rung-1 scope). This allocates ONE block
 * per control object, lazily, initialized from templ, and caches it in loc. That
 * is correct for a single-threaded image — which the RMS-substrate link proof
 * and the alpha port image are today. FULL per-thread storage (a pthread-key +
 * per-thread array, GCC's real emutls.c shape) is deferred to the rung-4
 * runtime proof (vms-f49), where a live /dev/vms executive and qemu-alpha can
 * actually exercise multi-threaded __thread; until then this is a documented,
 * tracked limitation, NOT faked per-thread isolation (INV-6). Building it here
 * blind (no way to run a multi-threaded alpha image on this host) would be
 * unvalidated; rung 4 forces and validates the per-thread version.
 */

#if defined(__alpha__)

/* Provided by DECC$SHR (musl) at activation. */
extern void *malloc(unsigned long);
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);

/* The control object the compiler emits, in emission order. `word` is pointer-
 * width (8 bytes on this LP64-kernel/LLP64-compiler target: the .quad fields
 * above are 64-bit). */
/* vms-f49 (rung 4): each control field the alpha-dec-vms cc1 emits is a `.quad`
 * (64-bit) -- `__emutls_v.x: .quad size ; .quad align ; .quad loc ; .quad templ`
 * (verified from generated assembly). emutls_word MUST therefore be 64-bit so
 * `loc` lands at struct offset 16 and `templ` at 24, matching the emission. It
 * was `unsigned long`, which on this LLP64 target is 32 BITS (same class as the
 * vms-1fc width bug) -- that packed size+align into the first 8 bytes and put
 * `loc` at offset 8 (the align field). __emutls_get_address then returned the
 * align value (4) as the TLS pointer, and the first __thread access on the
 * veneer's sys$create path dereferenced 4 -> SIGSEGV (the rung-4 blocker). Use
 * `unsigned long long` (guaranteed 64-bit on every target) to match the .quad. */
typedef unsigned long long emutls_word;
struct __emutls_object {
	emutls_word  size;   /* bytes of the __thread object  (.quad, offset 0)  */
	emutls_word  align;  /* required alignment            (.quad, offset 8)  */
	void        *loc;    /* runtime-owned storage pointer (.quad, offset 16) */
	void        *templ;  /* initializer image, 0=>zero    (.quad, offset 24) */
};

void *__emutls_get_address(struct __emutls_object *obj)
{
	void *p = obj->loc;
	if (p == 0) {
		unsigned long n = obj->size ? obj->size : 1;
		p = malloc(n);
		if (p == 0)
			return 0;               /* honest OOM; caller faults, not faked */
		if (obj->templ)
			memcpy(p, obj->templ, obj->size);
		else
			memset(p, 0, obj->size);
		obj->loc = p;                   /* cache (single-threaded model) */
	}
	return p;
}

#endif /* __alpha__ */
