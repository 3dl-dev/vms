/*
 * librarian_negctl.c - NEGATIVE CONTROL for the LIBRARIAN.EXE netbsd-vax
 * cross-compile (vms-9172, epic vms-509 "unified cross-platform build" Rung
 * E, docs/design-unified-cross-build.md §5/§8). Re-expresses the
 * CROSSCOMPILE_NEGCTL=1 branch formerly hand-rolled in the retired
 * tools/cross-vax/build-librarian-vax.sh (the per-image netbsd-vax CI gate
 * collapsed into the ovmx-images aggregate, vms-64a) -- equivalent
 * deliberately-broken-C content, moved to a checked-in fixture, compiled by
 * tests/netbsd/guest/negctl/run_negctl.sh under ctest instead of a
 * hand-invoked shell script that appended garbage to a copy of the live
 * librarian.c (a static fixture proves the same thing -- that the
 * elf32-vax cross-compile genuinely rejects malformed C -- without
 * embedding a copy of librarian.c that could silently drift from source).
 *
 * Deliberately WRONG: not valid C. If this TU compiles clean, the
 * cross-compile check that gates LIBRARIAN.EXE (and every other single-TU
 * image built through ovmx-images) has no teeth -- a genuine syntax/type
 * break would not red the PR.
 */
int foo(void) {
this is deliberately invalid C @@@ ;
}
