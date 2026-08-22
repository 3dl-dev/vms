/*
 * img.lds -- linker script for the minimal VMS-standard Alpha proof image
 * (bead vms-f60d). Produces a PT_INTERP image with NO PT_DYNAMIC (a symbol-
 * vector image: IMGACT reaches activate_symbol_vector), placing the OVMX
 * `.vms$*` carrier sections in a single writable PT_LOAD so IMGACT's
 * apply_vms_rel can bias the PDSC entry slot in memory.
 */
PHDRS {
  interp PT_INTERP FLAGS(4);
  load   PT_LOAD   FLAGS(7);   /* RWX: tiny self-contained proof image */
}
SECTIONS {
  . = 0x10000;
  .interp   : { *(.interp) }   :interp :load
  .text     : { *(.text) }     :load
  .data     : { *(.data) }     :load
  .vms$imp  : { *(.vms$imp) }  :load
  .vms$xfer : { *(.vms$xfer) } :load
  .vms$rel  : { *(.vms$rel) }  :load
  /DISCARD/ : { *(.note*) *(.comment) *(.gnu*) }
}
