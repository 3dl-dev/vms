#!/bin/bash
# test_telnet_no_libc_wait.sh (rd vms-a6b) -- the TELNET session loop and the
# TCP/IP client engine reach the network and the terminal ONLY through VMS
# system services ($ASSIGN/$QIOW, $SETIMR/$WAITFR). Baron 2026-11-02: "don't
# drop to linux LIBC, use all VMS-authentic API". Reds if a poll()/select()/
# read()/write()/recv()/send() or the BG readiness fd (vms_kif_bg_pollfd) comes
# back into either. Source audit of the TELNET section of dcl_cmd_misc.c and of
# the whole of tcpip_client.h (comments are stripped first).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MISC="${TELNET_AUDIT_MISC:-$ROOT/src/vmsdcl/dcl_cmd_misc.c}"
CLIENT="$ROOT/src/vmstcpip/services/tcpip_client.h"
FAIL=0
ok()  { echo "  PASS: $1"; }
bad() { echo "  FAIL: $1"; FAIL=1; }

strip() { # drop block and line comments
  sed -e ':a;s:/\*[^*]*\*\+\([^/*][^*]*\*\+\)*/::g;/\/\*/{N;ba}' -e 's://.*$::'
}
BANNED='\b(poll|ppoll|select|pselect|epoll_wait|read|write|recv|send|recvfrom|sendto|fcntl|ioctl)[[:space:]]*\(|vms_kif_bg_[a-z_]*|\bpollfd\b|STDIN_FILENO|<poll\.h>|<sys/select\.h>'

telnet=$(awk '/^#define TELNET_ESCAPE/{f=1} /^ \* The local side of an FTP transfer/{f=0} f' "$MISC")
[ -n "$telnet" ] && ok "found the TELNET section" || bad "TELNET section not found"
hits=$(printf '%s\n' "$telnet" | strip | grep -En "$BANNED")
[ -z "$hits" ] && ok "TELNET loop: no libc I/O wait, no executive-channel ioctl wrapper" || { bad "TELNET loop drops to libc:"; echo "$hits"; }
grep -q '^#include <poll.h>' "$MISC" && bad "dcl_cmd_misc.c still includes <poll.h>" || ok "dcl_cmd_misc.c does not include <poll.h>"
printf '%s\n' "$telnet" | grep -q 'IO\$M_NOW\|tcpip_recv_now' && ok "TELNET reads the connection with IO\$M_NOW" || bad "no IO\$M_NOW read"
printf '%s\n' "$telnet" | grep -q 'sys\$setimr' && printf '%s\n' "$telnet" | grep -q 'sys\$waitfr' \
  && ok "TELNET idles on a \$SETIMR/\$WAITFR event flag" || bad "no \$SETIMR/\$WAITFR idle wait"
hits=$(strip < "$CLIENT" | grep -En "$BANNED")
[ -z "$hits" ] && ok "tcpip_client.h: only \$ASSIGN/\$QIOW" || { bad "tcpip_client.h drops to libc:"; echo "$hits"; }
exit $FAIL
