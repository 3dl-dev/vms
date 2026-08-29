/* [OVMX] vms-095 (component C3 of vms-5f5) — object A of the local/weak module-GP
   K-resolution fixture (docs/design-alpha-per-image-gp.md 2.2.1).

   Covers three linkage-section-referencing procedures that a global-name-keyed K
   table could NOT resolve (each would `%LINK-F-GPDISPUNDEF` or bind the wrong K
   before the fix):
     (1) static_helper  — a STATIC/local proc (absent from the EVAX global symbol
         directory), like musl's io_thread_func;
     (2) shared_proc     — a WEAK def a strong def in object B overrides;
     (3) leaf_extern     — a PT_NULL-extern leaf (no call/frame, only an extern
         data ref -> still a linkage load -> still needs the module-GP).
   Rule-9-clean build/oracle tooling.  */

extern int ext_data;
extern int ext_call (int);

/* (1) STATIC/local, forced out-of-line + address-taken so it is emitted with its
   own PDSC and its own `.ovmx_gpdisp'. */
static int __attribute__((noinline))
static_helper (int x)
{
  return ext_call (x) + ext_data;
}

/* (2) WEAK def, referencing the linkage section; object B strongly overrides it. */
int __attribute__((weak))
shared_proc (int x)
{
  return ext_call (x) + ext_data;
}

/* (3) PT_NULL-extern leaf: no call, no frame, just an external data reference
   (gas turns `lda ext_data' into a linkage-section load). */
int
leaf_extern (void)
{
  return ext_data;
}

int (*take_static (void)) (int) { return static_helper; }

int
entry_a (int y)
{
  return static_helper (y) + shared_proc (y) + leaf_extern () + ext_data;
}
