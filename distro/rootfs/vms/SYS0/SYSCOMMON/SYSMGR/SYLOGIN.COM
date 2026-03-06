$! SYLOGIN.COM - System-wide login command procedure
$!
$! This file is executed for every interactive login.
$! Place system-wide settings and announcements here.
$!
$ WRITE SYS$OUTPUT "    Welcome to OVMX -- OpenVMS Compatible eXperience"
$ EXIT
