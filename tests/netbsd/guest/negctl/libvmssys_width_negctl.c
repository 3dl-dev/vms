/*
 * libvmssys_width_negctl.c - width-safety NEGATIVE CONTROL for the vax
 * toolchain itself (vms-9dc, epic vms-509 "unified cross-platform build"
 * Rung E, docs/design-unified-cross-build.md §5/§8). Re-expresses the
 * CROSSCOMPILE_NEGCTL=1 heredoc formerly hand-rolled in the retired
 * tools/cross-vax/build-libvmssys-vax.sh (the per-library netbsd-vax CI gate
 * collapsed into the ovmx-images aggregate, vms-64a) -- byte-identical
 * content, moved to a checked-in fixture, compiled by
 * tests/netbsd/guest/negctl/run_negctl.sh under ctest instead of a
 * hand-invoked shell script.
 *
 * Deliberately WRONG for the VAX (ILP32): these assert the LP64 widths. On a
 * real vax--netbsdelf compile long and void* are 32-bit, so both static
 * assertions MUST be hard errors. If this TU compiles clean, the ILP32 width
 * gate every ported layer relies on (docs/audit-ilp32-vax-libvmssys.md §1)
 * has no teeth -- it would pass code built with 64-bit pointer/long math.
 */
#include <stdint.h>
_Static_assert(sizeof(long)  == 8, "NEGCTL: pretend VAX long is 64-bit (LP64)");
_Static_assert(sizeof(void*) == 8, "NEGCTL: pretend VAX pointer is 64-bit (LP64)");
int main(void){ return 0; }
