/* [OVMX] vms-095 (component C3 of vms-5f5) — test TU for the OVMX per-image
   module-GP prologue (docs/design-alpha-per-image-gp.md 2.1/2.1.1).

   A multi-procedure translation unit whose procedures reference external
   symbols and call one another, so each is a stack procedure with linkage-
   section loads.  run_module_gp_proof.sh compiles this with the port cc1 and
   objdump-asserts that every procedure:
     - establishes its module-GP in $15 (the .ovmx_gpdisp $15 expansion,
       ldah $15,0($27) / lda $15,0($15)),
     - SAVES the caller's $15 in the prologue and RESTORES it in the epilogue
       (mandatory: $15 is callee-saved and holds the caller's own module-GP; a
       cross-module return would otherwise resume with the wrong linkage base),
     - addresses the linkage section $15-relative (not $27/&PDSC, the N=7 skew).

   Not run inside the guest; Rule-9-clean build/oracle tooling.  */

extern int ext_alpha (int);
extern int ext_beta (int);
extern int ext_gamma;

int
mgp_leaf (int x)
{
  return ext_alpha (x) + ext_gamma;
}

int
mgp_middle (int y)
{
  return ext_beta (y) * ext_gamma + mgp_leaf (y);
}

int
mgp_top (int z)
{
  return mgp_middle (z) - ext_alpha (z) + mgp_leaf (z);
}
