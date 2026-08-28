/*
 * img.lds -- linker script for the minimal VMS-standard Alpha proof image
 * (bead vms-f60d). Produces a PT_INTERP image with NO PT_DYNAMIC (a symbol-
 * vector image: IMGACT reaches activate_symbol_vector), placing the OVMX
 * `.vms$*` carrier sections in a single writable PT_LOAD so IMGACT's
 * apply_vms_rel can bias the PDSC entry slot in memory.
 *
 * The ELF header + program headers MUST live inside the PT_LOAD (via FILEHDR
 * PHDRS on the load segment) and be described by a PT_PHDR segment, so the real
 * kernel sets AT_PHDR to a MAPPED address. Without this the hand-crafted stub
 * leaves AT_PHDR pointing at an unmapped page and IMGACT segfaults on the first
 * subject-phdr read in exec_bias() (vms-f60d): qemu-user leniently maps the
 * whole file so it survives, but qemu-system-alpha's kernel maps only PT_LOAD
 * ranges. A genuine GCC-port ET_DYN image already carries PT_PHDR + mapped
 * headers -- this hand-written stub was the only anomaly.
 */
PHDRS {
  headers PT_PHDR   PHDRS FLAGS(4);
  interp  PT_INTERP       FLAGS(4);
  load    PT_LOAD   FILEHDR PHDRS FLAGS(7);   /* RWX; FILEHDR+PHDRS map ehdr+phdrs */
}
SECTIONS {
  . = 0x10000 + SIZEOF_HEADERS;
  .interp   : { *(.interp) }   :load :interp
  .text     : { *(.text) }     :load
  .data     : { *(.data) }     :load
  .vms$imp  : { *(.vms$imp) }  :load
  .vms$xfer : { *(.vms$xfer) } :load
  .vms$rel  : { *(.vms$rel) }  :load
  /DISCARD/ : { *(.note*) *(.comment) *(.gnu*) }
}
