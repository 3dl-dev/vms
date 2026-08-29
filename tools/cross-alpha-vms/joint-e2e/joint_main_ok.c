/* joint_main_ok.c (vms-8208) -- the value-sensitivity CONTROL for the module-GP
 * API-compat activation gate.
 *
 * Identical in every structural respect to joint_main.c (same real port crt0,
 * same genuine alpha DECC$SHR, same single-proc __main image), EXCEPT main
 * returns 0 instead of the sentinel 3. On the real OVMX/Alpha executive that
 * must read back the success-class $STATUS SS$_NORMAL (0x00000001), whereas
 * joint_main.c's `return 3` reads C$_EXIT1+(3-1)*8 = 0x0035A019 (N=3).
 *
 * The pair is the gate's teeth: a fixed constant, or a failed activation that
 * never runs main, cannot satisfy BOTH the N=3 decode (milestone) AND the
 * SS$_NORMAL anchor (this control) at once. The distinct crt0-join banner
 * ("OK-CONTROL") lets the harness attribute each seam line to the right image.
 *
 * Compiled fresh by the SAME real alpha-dec-vms cc1 (-mpointer-size=64) as
 * joint_main.c via build-joint-image.sh JOINT_MAIN=joint_main_ok.c.
 */
extern int printf(const char *, ...);
int main(int argc, char **argv, char **envp) {
    (void)argv; (void)envp;
    printf("OVMX crt0 join OK-CONTROL: activated, argc=%d\n", argc);
    return 0;   /* success -> $STATUS reads SS$_NORMAL (0x1): the anchor */
}
