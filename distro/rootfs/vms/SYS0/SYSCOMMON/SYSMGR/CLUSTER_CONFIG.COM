$! CLUSTER_CONFIG.COM - OpenVMX cluster configuration entry point (vms-258)
$!
$! OVMX clusters over raw-Ethernet SCA (a LAN interconnect), so the LAN
$! configuration procedure is the one that applies. On VMS a LAN-only system
$! is likewise directed to CLUSTER_CONFIG_LAN.COM. An operator who types either
$! name lands in the same place: this wrapper dispatches to the LAN procedure.
$! See CLUSTER_CONFIG_LAN.COM for the provenance and honesty notes.
$!
$ SET NOON
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    OpenVMX uses a LAN (Ethernet) cluster interconnect."
$ WRITE SYS$OUTPUT "    Invoking SYS$MANAGER:CLUSTER_CONFIG_LAN.COM ...."
$ @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM
$ EXIT
