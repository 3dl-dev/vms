$! STARTUP.COM - OVMX System Startup Procedure
$!
$! Executed by OVMX_INIT during system boot.
$! Add site-specific startup commands below.
$!
$ WRITE SYS$OUTPUT "%STARTUP-I-BEGIN, system startup begun"
$! Future: start system processes, mount disks, configure network
$ WRITE SYS$OUTPUT "%STARTUP-I-COMPLETE, system startup completed"
$ EXIT
