/*
 * mbx_negctl.c - width-safety NEGATIVE CONTROL for the mailbox guest tool
 * (vmsmbx.c), re-expressed as a ctest build-negative (vms-74b, epic vms-509
 * "unified cross-platform build" Rung D). Formerly the
 * CROSSCOMPILE_NEGCTL=1 heredoc in tools/cross-vax/build-mbx-tool-vax.sh
 * (rd vms-fe8) -- moved to a checked-in fixture, byte-identical content,
 * compiled by tests/netbsd/guest/negctl/run_negctl.sh under ctest instead of
 * a hand-invoked shell script.
 *
 * Deliberately WRONG: pretends the mailbox message length is a 64-bit long
 * (an LP64 assumption). On vax (ILP32) long is 32-bit, so this static
 * assertion must be a hard compile error -- if it is not, the width gate
 * this probe depends on (vms_mbx_nb.h's uint32_t chan/len/status fields) has
 * no teeth.
 */
#include <stdint.h>
_Static_assert(sizeof(long) == 8, "NEGCTL: pretend vax long is 64-bit (LP64)");
int main(void){ return 0; }
