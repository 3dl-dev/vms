# Semantic-oracle probes LINKed on the lab Alpha (not yet in the native gate)

Captured 2026-10-09 on the ovmx-lab OpenVMS Alpha V8.4 node (pod n3b3f-alpha) by
the vms-3b3f capture recipe: `spgen.py --mar` for the family, then the node's own
MACRO, LINK/MAP/FULL/CROSS, RUN, and DUMP of the linked image (the .EXE here is
rebuilt from that DUMP). Each RUN transcript equals
`docs/oracle/semantics/<family>/alpha84.txt` (chkpro: `vax73.txt`).

They join the native gate (`tests/native-images/alpha/sp/`) once the services they
call are vectored for native images: the rights database, RMS and $CHKPRO in the
VMS-ABI producers (rd vms-8b5, vms-e1a7).

SP_PRC ran in the gate briefly: with $CREPRC unavailable in the VMS-ABI build
(vms-55f6) its later cases act on PID 0, the probe itself, so $SUSPND/$DELPRC
stop it at a point that varies from run to run. It returns to the gate with
vms-55f6.
