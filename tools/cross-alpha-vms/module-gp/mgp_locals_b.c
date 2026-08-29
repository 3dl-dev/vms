/* [OVMX] vms-095 — object B: the STRONG definition of shared_proc that overrides
   object A's WEAK one.  Also references the linkage section, so it too carries a
   `.ovmx_gpdisp'.  Its (global) PDSC is the surviving definition, so a gpdisp
   naming shared_proc (A's weak-def site, redirected strong-over-weak; and B's own
   strong-def site) resolves K to THIS descriptor's placement.  B also DEFINES the
   externals A references, so A's calls/data refs stay real cross-object linkage
   loads and the shareable links with no undefined symbols. */
int ext_data = 42;
int ext_call (int x) { return x + 1; }

int
shared_proc (int x)
{
  return ext_call (x) * 3 + ext_data;
}
