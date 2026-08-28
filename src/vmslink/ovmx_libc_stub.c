/* OVMX loader glue (vms-c07): expose musl's hidden `__libc` so IMGACT — the
 * PT_INTERP for LINK.EXE images — can set up an OVMX executable's OWN main-
 * program TLS.
 *
 * WHY THIS EXISTS. musl-as-a-shareable (DECC$SHR whole-archives musl's libc.a)
 * cannot compute an OVMX executable's TLS load bias: static_init_tls() derives
 * the main-program bias only from PT_DYNAMIC, which for a LINK.EXE image with
 * AT_BASE != 0 resolves to DECC$SHR's own _DYNAMIC (bias 0) — so the exe's
 * .tdata image address comes out unrelocated and __copy_tls faults. IMGACT
 * already holds the real bias (g_exe.tls_image = exe_base + p_vaddr), so it
 * drives musl's OWN __copy_tls/__init_tp with a relocated module. To poke the
 * few __libc TLS fields (tls_head/tls_cnt/tls_size/tls_align) it needs the
 * address of musl's `__libc`, which musl marks hidden. This getter — a plain
 * PROCEDURE universal — hands that address across the shareable boundary.
 *
 * A getter PROCEDURE, not a `__libc=DATA` universal on purpose: `__libc` lives
 * in BSS, and LINK.EXE's emit_shareable() resolves the symbol vector BEFORE it
 * places BSS-bucket sections, so a BSS DATA universal dies "universal symbol
 * not defined" with sec_va==0 (the environ/BSS-ordering gap, vms-910). A
 * PROCEDURE universal sidesteps that entirely.
 */
extern char __libc[] __attribute__((visibility("hidden")));

void *ovmx_get_libc(void)
{
	return (void *)__libc;
}
