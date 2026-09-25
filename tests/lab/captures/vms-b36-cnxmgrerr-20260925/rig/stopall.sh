#!/bin/bash
R=/lab/run-b36
SELF=$$
kill_pat() {
  for p in $(ps -eo pid,args | grep -E "$1" | grep -v grep | grep -v stopall | awk '{print $1}'); do
    [ "$p" = "$SELF" ] && continue
    kill -9 "$p" 2>/dev/null
  done
}
kill_pat "bash /lab/run-b36/loop[.]sh"
kill_pat "bash /lab/run-b36/runarm[.]sh"
kill_pat "bash /lab/run-b36/blackout[.]sh"
kill_pat "bash /lab/run-b36/b36node[.]sh"
kill_pat "qemu-system-x86_64 .*brb36|qemu-system-x86_64 .*tapAb36|qemu-system-x86_64 .*tapBb36"
kill_pat "nodedrv[.]py /lab/run-b36"
kill_pat "tcpdum[p] -i brb36"
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 "$p" 2>/dev/null; done
tc qdisc del dev tapBb36 root 2>/dev/null
sleep 3
echo "remaining:"; ps -eo pid,args | grep -E "qemu-system-x86_64 |b36node|loop[.]sh" | grep -v grep | grep -v defunct | grep -v stopall | wc -l
