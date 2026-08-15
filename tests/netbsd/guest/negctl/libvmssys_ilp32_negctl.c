/*
 * libvmssys_ilp32_negctl.c - width-safety NEGATIVE CONTROL for the vax
 * toolchain itself (vms-9dc, epic vms-509 "unified cross-platform build"
 * Rung E, docs/design-unified-cross-build.md §5/§8). Re-expresses the
 * CROSSCOMPILE_NEGCTL=1 heredoc formerly hand-rolled in the retired
 * tools/cross-vax/build-libvmssys-vax.sh (the per-library netbsd-vax CI gate
 * collapsed into the ovmx-images aggregate, vms-64a) -- byte-identical
 * content, moved to a checked-in fixture, compiled by
 * tests/netbsd/guest/negctl/run_negctl.sh under ctest instead of a
 * hand-invoked shell script.
 *
 * Named "*_ilp32_negctl_vax" rather than "*_width_negctl_vax" (unlike the
 * four facility fixtures) so it does NOT match
 * build-facility-tools-vax-cmake.sh's `ctest -R '_width_negctl_vax$'`
 * selector -- that job asserts an exact count of 4 facility tests, and this
 * fixture is not one of them (a real collision hit in CI review of vms-08cb).
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
