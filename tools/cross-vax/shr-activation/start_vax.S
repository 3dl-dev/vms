/*
 * start_vax.S -- vms-d4a (vms-404 P4) CONSUMER.EXE freestanding entry stub.
 *
 * WHY A HAND-WRITTEN _start (not LINKVAX.EXE's synthesized crt0). This gate's
 * whole point is that CONSUMER.EXE's ONLY cross-image import is
 * `purdy_s_hash` from the SHIPPED, RESIDENT LIBVMS$SHR.EXE (rd vms-c7f7) --
 * nothing from a gate-private producer. LINKVAX.EXE's synthesized crt0 (for a
 * `main()` program) force-binds `exit` as an import from a --use producer
 * (src/vmslink/link.c: "--executable needs exit() from a --use producer
 * (DECC$SHR)"); on the VAX Decision-A substrate there is NO resident OVMX
 * C-RTL producer to satisfy it (the shipped VAX images are ordinary ld.elf_so
 * dynamic executables, build-ovmx-images-vax-cmake.sh), which is exactly why
 * the prior cut leaned on a gate-private P4BOOT$SHR.EXE. Defining our OWN
 * `_start` makes CONSUMER FREESTANDING in LINKVAX's sense (link.c: "An object
 * set that defines its own `_start` ... owns entry + exit ... no crt0, no
 * C-RTL exit import"), so the deferred/undefined C-RTL symbols the statically
 * pulled NetBSD libc.a leaves (printf's emutls errno, environ, ...) are the
 * only externals -- purdy_s_hash stays the single .vms$imp cross-image bind.
 *
 * IMGACT.EXE enters this image SysV-flavor -- a bare tail-JMP to e_entry with
 * argc/argv/envp/auxv on SP and NO CALLS frame (src/imgact/arch/vax/start.S:
 * "mirror x86_64/aarch64's SysV tail-jump ... jmp (%r0)"). So `_start` must be
 * a bare label with NO VAX entry mask (a C function's `.word <mask>` prologue,
 * jumped into rather than CALLS'd, would be mis-executed as its first
 * instruction). It immediately CALLS the C body (which HAS an entry mask, so
 * the VAX calling convention is honoured from here down) and CALLS the libc
 * `_exit` with the body's return value so DCL's fork()-path reads a POSIX exit
 * code it maps to $STATUS (dcl_cmd_process.c: exit 0 -> SS$_NORMAL, nonzero ->
 * %DCL-E-ABORT + SS$_ABORT). stdout is flushed by the body before it returns.
 *
 * Clean-room (Rule 8): the VAX CALLS / entry-mask conventions are the public
 * VAX architecture conventions, the same ones src/imgact/arch/vax/start.S
 * documents against the cross toolchain's own libc stubs.
 */
	.text
	.globl _start
	.type _start,@function
_start:
	calls	$0, consumer_body	/* r0 = consumer_body() return value       */
	pushl	%r0			/* arg1 = exit code                        */
	calls	$1, _exit		/* libc _exit(2): terminate the image      */
	halt				/* unreachable -- _exit never returns      */
	.size _start,.-_start
