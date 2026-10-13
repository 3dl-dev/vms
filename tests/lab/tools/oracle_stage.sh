#!/bin/bash
# oracle_stage.sh -- runs INSIDE a lab pod (copy it in, then
# `kubectl -n ovmx-lab exec <pod> -- bash /lab/run-oracle/oracle_stage.sh ...`).
# Stages a real-VAX-only oracle arm one step at a time, so a node can boot
# conversationally (SYSBOOT> SET ...) or late, on the pod's own br0.
#
#   oracle_stage.sh cap <tag>          start an 0x6007 capture: /lab/run-oracle/<tag>.pcap
#   oracle_stage.sh stopcap            stop this pod's oracle capture (flushes the pcap)
#   oracle_stage.sh node <vaxN> [conv] boot vax2 (SYS1) or vax3 (SYS2) on its own tap;
#                                      'conv' stops at SYSBOOT> (R5 bit 0)
#
# The pod's entrypoint boots vax1 (NODES="vax1"); this adds the others. Each node
# gets its own nvram and the pod's own cloned d0/d1 (one shared system disk,
# one root per node, as the golden 3-node volume is laid out).
set -u
R=/lab/run-oracle
LAB_DIR=/lab/k8s-labs/${POD_NAME:?POD_NAME}
mkdir -p "$R"
case "${1:-}" in
cap)
	TAG=${2:?tag}
	setsid nohup tcpdump -i br0 -s0 -U -w "$R/$TAG.pcap" 'ether proto 0x6007' \
		> "$R/$TAG.tcpdump.out" 2>&1 < /dev/null &
	echo "capturing $TAG" ;;
stopcap)
	for p in $(ps -eo pid,args | grep "tcpdum[p] -i br0 -s0 -U -w $R/" | awk '{print $1}'); do
		kill -INT "$p"
	done
	echo "stopped" ;;
node)
	n=${2:?node}; conv=${3:-}
	case "$n" in
	vax2) tap=tap2; dz=2002; root=1 ;;
	vax3) tap=tap3; dz=2003; root=2 ;;
	*) echo "oracle_stage.sh: node is vax2 or vax3" >&2; exit 2 ;;
	esac
	r5=$(printf '%X0000000' "$root")
	[ "$conv" = conv ] && r5=$(printf '%X0000001' "$root")
	nd=$LAB_DIR/$n
	ip tuntap add dev "$tap" mode tap 2>/dev/null
	ip link set "$tap" master br0
	ip link set "$tap" up
	mkdir -p "$nd/data"
	[ -f "$nd/data/nvram.bin" ] || cp "/lab/cluster/$n/data/nvram.bin" "$nd/data/nvram.bin" 2>/dev/null \
		|| cp /lab/cluster/data/nvram.bin "$nd/data/nvram.bin"
	ln -sf /usr/local/bin/vax "$nd/vax"
	sed "s|$LAB_DIR/vax1/data/nvram.bin|$nd/data/nvram.bin|" "$LAB_DIR/vax1/vax.ini" > "$nd/vax.ini"
	printf 'set xq enable\nat xq tap:%s\natt dz %s,speed=*32\n' "$tap" "$dz" > "$nd/local.ini"
	touch "$LAB_DIR/logs/$n.log"
	D=$(date -u +"%d-%b-%Y %H:%M" | tr a-z A-Z)
	setsid nohup python3 /usr/local/bin/nodedrv.py "$nd" "$LAB_DIR/logs/$n.log" --no-detach \
		--date "$D" --boot "B/R5:$r5 DUA0" > "$R/$n.drv.out" 2>&1 < /dev/null &
	echo "booting $n: B/R5:$r5 DUA0" ;;
*)
	sed -n '2,13p' "$0"; exit 2 ;;
esac
