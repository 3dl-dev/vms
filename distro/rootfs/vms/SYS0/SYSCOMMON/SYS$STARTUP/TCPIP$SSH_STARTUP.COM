$! TCPIP$SSH_STARTUP.COM - OVMX SSH server (VMSSSHD) startup procedure (rd vms-843a).
$!
$! On real OpenVMS, TCP/IP Services' SSH server runs as a PERSISTENT DETACHED
$! DAEMON started by its own startup procedure -- NOT as an auxiliary (inetd)
$! service. (The auxiliary server TCPIP$INETD carries the simple accept-handoff
$! wait-model services like DAYTIME; SSH, with long-lived interactive sessions and
$! its own concurrency, is a standalone daemon.) This procedure is the OVMX
$! equivalent: it starts VMSSSHD.EXE -- the wrapped, unmodified OpenSSH sshd -- as
$! a detached process. sshd itself binds :22 and listen/accepts over the executive
$! BGn: seam (the vms-0cd/vms-9cc-proven server transport, ovmx_socket/bind/listen/
$! accept), authenticates passwords against the binary SYSUAF (Purdy), and drops
$! the session into LOGINOUT -> DCL.
$!
$! ARGS. OVMX DCL RUN cannot pass Unix argv (its RUN Image qualifiers are
$! oracle-verbatim: /DEBUG, /NODEBUG only), so VMSSSHD.EXE is launched argv-less
$! and its ovmx wrap (__wrap_main, third-party/openssh/ovmx/ovmx_ssh_wrap.c)
$! injects the daemon args `-D -e -f /ovmxsshsrv/etc/sshd_config`: -D keeps sshd in
$! the foreground of its detached process (no double-daemonize), -e logs to stderr,
$! and -f names the sshd_config the SSH overlay generated on the initramfs.
$!
$! INV-6 (Rule 9): sshd's listener binds over the executive BGn: device. With no
$! executive (/dev/vms absent) the wrapped socket()/bind() fail honestly
$! (SS$_NOSUCHDEV -> ENODEV) and VMSSSHD exits non-zero -- it never fabricates a
$! per-process listener that shares nothing.
$!
$! I/O goes to OPA0: (the operator console) so the daemon's -e log is visible on
$! the console -- the SSH wire data rides the BGn: socket sshd accepts, NOT this
$! stdout/stderr, so console logging cannot corrupt a session. (A production
$! deployment would point these at SYS$MANAGER:VMSSSHD.LOG instead; the SSH
$! cold-boot proof wants the log on the console it captures.)
$!
$! INVOCATION:
$!   @SYS$STARTUP:TCPIP$SSH_STARTUP
$!
$! POSTURE (Baron-reserved): SSH is a layered product, OFF by default. The shipped
$! SYSTARTUP_VMS.COM does NOT invoke this procedure (the posture guard enforces
$! that); only the SSH test overlay's SYSTARTUP does. SSH-enable-by-default + a
$! shipped host key are operator posture decisions.
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$ SAY ""
$ SAY "        OVMX SSH Server -- Startup (TCPIP$SSH_STARTUP)"
$ SAY ""
$ VMSSSHD_IMAGE = "SYS$SYSTEM:VMSSSHD.EXE"
$! The SSH server is a persistent listener -- it MUST run DETACHED (a foreground
$! RUN would block system startup forever). The EXECUTIVE assigns the process name
$! VMSSSHD as it creates the process, so SHOW SYSTEM lists it and a duplicate is
$! refused to the caller (%RUN-F-CREPRC / -SYSTEM-F-DUPLNAM), making a re-run
$! idempotent-safe. No image argv is given here; __wrap_main supplies -D -e -f.
$ SAY "%SSH-I-STARTSRV, starting the SSH server (VMSSSHD) from ''VMSSSHD_IMAGE'"
$ RUN /DETACHED -
       /PROCESS_NAME=VMSSSHD -
       /INPUT=NL: /OUTPUT=OPA0: /ERROR=OPA0: -
       ''VMSSSHD_IMAGE'
$ SAY "%SSH-I-SRVSTARTED, SSH server (VMSSSHD) started detached"
$ EXIT
