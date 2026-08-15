/*
 * access_negctl.c - width-safety NEGATIVE CONTROL for the access-mode +
 * cross-process AST guest tool (vmsaccess.c), re-expressed as a ctest
 * build-negative (vms-74b, epic vms-509 "unified cross-platform build"
 * Rung D). Formerly the CROSSCOMPILE_NEGCTL=1 heredoc in
 * tools/cross-vax/build-access-tool-vax.sh (rd vms-4b7) -- moved to a
 * checked-in fixture, byte-identical content, compiled by
 * tests/netbsd/guest/negctl/run_negctl.sh under ctest instead of a
 * hand-invoked shell script.
 *
 * Deliberately WRONG: pretends astprm/perm_privs's underlying `long' is
 * 64-bit (an LP64 assumption). On vax (ILP32) long is 32-bit, so this static
 * assertion must be a hard compile error -- if it is not, the width gate this
 * probe depends on (vms_ast_nb.h's / vms_access_nb.h's explicit uint64_t
 * astadr/astprm/cur_privs/perm_privs fields, never a bare `long') has no
 * teeth.
 */
#include <stdint.h>
_Static_assert(sizeof(long) == 8, "NEGCTL: pretend vax long is 64-bit (LP64)");
int main(void){ return 0; }
