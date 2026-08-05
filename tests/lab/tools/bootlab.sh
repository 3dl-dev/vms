#!/bin/bash
# bootlab.sh -- boot the 3-node lab from the LIVE disks (no golden restore).
# vms-2f3 workshop bring-up 2026-08-02: the 3-node golden .bak files did not
# survive the host migration, so reset3.sh's restore step cannot run. This is
# reset3.sh steps 3-6 with the restore removed. Use it to bring the lab UP; it
# does NOT clear stale CSBs. The 4-minute reproducer needs no reset.
set -u
CL=/home/baron/vax/cluster
T=/tmp/clean-vax1-test
STAT=$CL/work/bootlab.status
mkdir -p "$T" "$CL/work"
: > "$STAT"
log(){ echo "[$(date +%T)] $*" | tee -a "$STAT"; }
clean(){ tr -d '\000' <"$1" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }
# vms-d3a: nodedrv.py writes <log>.bootfail when the boot command was not
# echo-verified, or when the console auto-booted its own default root. Either
# way the node's identity/system root is NOT trustworthy -- on the shared
# d0.dsk that is how a node comes up as a duplicate VAX1. Never silent.
bootchk(){ if [ -e "$1.bootfail" ]; then
             log "!! $2 BOOT NOT TRUSTED -- $(tr '\n' ' ' <"$1.bootfail")"
             BOOTBAD="${BOOTBAD:-} $2"
           else log "$2 boot injection verified"; fi; }
BOOTBAD=""

log "stopping any nodedrv + SIMH"
for pid in $(pgrep -x python3); do
  c=$(tr '\0' ' ' </proc/$pid/cmdline 2>/dev/null)
  case "$c" in *nodedrv*) kill "$pid" 2>/dev/null;; esac
done
sleep 1
for pid in $(pgrep -x vax); do kill "$pid" 2>/dev/null; done
sleep 2
for pid in $(pgrep -x vax); do sudo kill -9 "$pid" 2>/dev/null; done
sleep 1
log "SIMH remaining: $(pgrep -x vax | wc -l)"

( cd "$CL" && sudo sh netup-capture.sh >/dev/null 2>&1 )
sudo ip tuntap add dev tap3 mode tap user baron 2>/dev/null
sudo ip link set tap3 master br0 2>/dev/null; sudo ip link set tap3 up 2>/dev/null
sudo ip link set br0 up 2>/dev/null
log "bridge up"

DATE="${LABDATE:-2-AUG-2026 12:00}"
: > "$T/vax1.log"
( cd "$CL" && nohup python3 nodedrv.py vax1 "$T/vax1.log" --date "$DATE" >"$T/nd-v1.out" 2>&1 & )
log "vax1 booting"
for i in $(seq 1 60); do clean "$T/vax1.log" | grep -qE 'Username:' && { log "vax1 login prompt ~$((i*4))s"; break; }; sleep 4; done
bootchk "$T/vax1.log" vax1
printf "SYSTEM\r" > "$T/vax1.log.in"; sleep 4
for i in $(seq 1 10); do clean "$T/vax1.log" | tail -3 | grep -qE 'Password:' && break; sleep 2; done
printf 'system\r' > "$T/vax1.log.in"; sleep 12
for i in $(seq 1 10); do
  printf 'WRITE SYS$OUTPUT "LOGIN_OK"\r' > "$T/vax1.log.in"; sleep 3
  clean "$T/vax1.log" | grep -q 'LOGIN_OK' && { log "vax1 DCL reached"; break; }
done
printf 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST\r' > "$T/vax1.log.in"; sleep 2
printf 'REPLY/ENABLE=(CLUSTER)\r' > "$T/vax1.log.in"; sleep 2

: > "$T/vax2.log"
( cd "$CL" && nohup python3 nodedrv.py vax2 "$T/vax2.log" --date "$DATE" \
    --boot "B/R5:10000000 DUA0" >"$T/nd-v2.out" 2>&1 & )
log "vax2 booting"
sleep 100
bootchk "$T/vax2.log" vax2
: > "$T/vax3.log"
( cd "$CL" && nohup python3 nodedrv.py vax3 "$T/vax3.log" --date "$DATE" \
    --boot "B/R5:20000000 DUA0" >"$T/nd-v3.out" 2>&1 & )
log "vax3 booting"
sleep 110
bootchk "$T/vax3.log" vax3
[ -n "$BOOTBAD" ] && log "!! UNTRUSTED BOOTS:$BOOTBAD -- do NOT treat this lab as a clean control"

printf 'WRITE SYS$OUTPUT "===BOOTLAB-VERIFY-BEGIN==="\r' > "$T/vax1.log.in"; sleep 2
printf 'WRITE SYS$OUTPUT "CN_" + F$STRING(F$GETSYI("CLUSTER_NODES"))\r' > "$T/vax1.log.in"; sleep 3
printf 'SHOW CLUSTER\r' > "$T/vax1.log.in"; sleep 8; printf '\032' > "$T/vax1.log.in"; sleep 2
printf 'WRITE SYS$OUTPUT "===BOOTLAB-VERIFY-END==="\r' > "$T/vax1.log.in"; sleep 2
log "nodes: $(clean "$T/vax1.log" | grep -aoE 'CN_[0-9]+' | tail -1)  simh=$(pgrep -x vax | wc -l)"
clean "$T/vax1.log" | sed -n '/===BOOTLAB-VERIFY-BEGIN===/,/===BOOTLAB-VERIFY-END===/p' | tee -a "$STAT"
echo "===BOOTLAB-DONE===" >> "$STAT"
