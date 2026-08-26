/* joint_main.c — the trivial main() of the minimal GENUINE alpha-dec-vms port
 * image (vms-864 link proof AND vms-f60d run proof).
 *
 * Built fresh by the REAL alpha-dec-vms cc1 and linked (via LINK.EXE, .vms$xfer
 * flavor=VMS_STD, EM_ALPHA) with the genuine port crt0 (__main, from the GCC
 * port's vms-ucrt0.c) and decc$main from the genuine alpha DECC$SHR.
 *
 * vms-f60d SENTINEL CONTRACT — the e2e proof that IMGACT's VMS-standard
 * activation actually RUNS a genuine port image end to end (activate through
 * the 6-arg Alpha standard call -> __main -> decc$main -> main() runs -> return
 * -> R0=condition -> IMGACT $EXIT/$STATUS), not just that the crt0 links:
 *
 *   main() returns  OVMX_SENTINEL | (argc & 0xFF)
 *
 * with OVMX_SENTINEL = 0x0FAC0000.  For a no-argument activation argc == 1, so
 *   $STATUS == 0x0FAC0001
 * which the IMGACT-side assertion (peer) checks.  Properties, by construction:
 *   - 0x0FAC in the high half is a distinctive marker unlikely to collide with
 *     a real SS$_/condition value, so a match proves THIS image ran (not some
 *     incidental status from a failed/short-circuited activation);
 *   - the low byte carries argc, so a correct value ALSO proves decc$main
 *     synthesised the argument vector from the VMS AI arg block and handed it to
 *     main() — i.e. the whole 6-arg -> decc$main -> argc chain, not just "some
 *     code ran";
 *   - bits <2:0> = 001 => VMS severity SUCCESS (odd status), so if the readback
 *     path only preserves severity the image still exits "success", and the
 *     console line below is the width-independent human-visible proof.
 *
 * If the IMGACT $STATUS readback cannot preserve a full longword, the assertion
 * can fall back to (a) the console line and (b) severity=SUCCESS; the sentinel
 * value is then trivially adjustable here — but keep this file and the peer's
 * assertion in agreement (that is the whole point of the contract).
 */

extern int printf(const char *, ...);

#define OVMX_SENTINEL 0x0FAC0000

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;

    int status = OVMX_SENTINEL | (argc & 0xFF);

    /* Width-independent, human-visible proof on the console: main() was
     * reached under VMS-standard activation with a real argument count. */
    printf("OVMX VMS-STD activation: main() reached, argc=%d, returning $STATUS=0x%08X\n",
           argc, status);

    return status;   /* -> R0=condition -> IMGACT $EXIT/$STATUS (sentinel) */
}
