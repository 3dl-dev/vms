#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# cluster_core_includes_gate.sh - the CI gate that keeps the executive-resident
# VMScluster stack SUBSTRATE-FREE (FC-P0.1).
#
# Design: docs/design-faithful-cluster-executive.md SS3.2.2 -- "a rule that
# kernel-core cluster code includes ONLY exec_kbackend.h and kernel-core
# headers", with the leak table naming exactly what must never appear
# (sk_buff/mbuf, softirq/callout, IPL, net_device/ifnet, LP64/ILP32 assumptions)
# and SS3.9 rule 3 ("no substrate include in any _fsm.c/codec TU").
#
# WHY A GATE AND NOT A REVIEW RULE. The seam's whole value is that ~13 500 lines
# of cluster code compile unchanged into both kmods, and the way that value is
# lost is one #include that only one substrate has. The precedent is in the tree:
# vms_l2.c is Linux-only and absent from the NetBSD module's SRCS -- exactly the
# outcome "Linux first, NetBSD later" produces. This gate is cheap, runs in
# seconds on every PR, and has a negative control (--self-test) so it can never
# pass by having no teeth.
#
# USAGE
#   tools/ci/cluster_core_includes_gate.sh            scan the repository
#   tools/ci/cluster_core_includes_gate.sh --self-test   prove the gate has teeth
#   OVMX_REPO=<dir> tools/ci/cluster_core_includes_gate.sh   scan another tree
#
# Exit 0 = clean. Exit 1 = a violation (each one printed with file:line and the
# rule it broke). Exit 2 = the gate could not run (no files matched -- which is
# itself a failure: a gate that scans nothing proves nothing).
#
# POSIX sh: no bashisms, so it runs under dash on a minimal CI image.

set -eu

REPO="${OVMX_REPO:-$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
CORE="$REPO/src/kernel-core"

# ---------------------------------------------------------------------------
# The file set. These are the kernel-core cluster translation units and headers
# named in design SS3.2.2. Sources that are NOT cluster code (vms_lock.c,
# vms_devtab.c, ...) are deliberately out of scope: they predate the seam and
# have their own portability discipline.
# ---------------------------------------------------------------------------
cluster_files() {
	# shellcheck disable=SC2039  # `find -name` patterns only; no bashisms
	find "$CORE" -maxdepth 1 -type f \
		\( -name 'vms_pe*.c'      -o -name 'vms_pe*.h' \
		-o -name 'vms_scs*.c'     -o -name 'vms_scs*.h' \
		-o -name 'vms_cnxman*.c'  -o -name 'vms_cnxman*.h' \
		-o -name 'vms_dlm_scs*.c' -o -name 'vms_dlm_scs*.h' \
		-o -name 'vms_mscp_*.c'   -o -name 'vms_mscp_*.h' \
		-o -name 'vms_cluster*.c' -o -name 'vms_cluster*.h' \) \
		| sort
}

# Strip C comments and preprocessor-free prose so the identifier rules below do
# not fire on documentation. (A doc-comment legitimately says "Linux:
# dev_add_pack" -- that is the seam being explained, not a leak.) Line numbering
# is preserved: comment bodies become blank lines.
strip_comments() {
	awk '
	{
		line = $0; out = ""; i = 1; n = length(line)
		while (i <= n) {
			c = substr(line, i, 1); d = substr(line, i, 2)
			if (in_block) {
				if (d == "*/") { in_block = 0; i += 2 } else { i++ }
				continue
			}
			if (d == "/*") { in_block = 1; i += 2; continue }
			if (d == "//") { break }
			out = out c; i++
		}
		print out
	}' "$1"
}

rc=0
violations=0

fail() {
	# fail <file> <line> <rule> <detail>
	echo "::error file=$1,line=$2::[$3] $4"
	violations=$((violations + 1))
	rc=1
}

# ---------------------------------------------------------------------------
# RULE 1 -- no angle-bracket include, EXCEPT the freestanding C standard
# headers a pure header's host branch (-DOVMX_CLUSTER_HOST) needs.
#
# Every HOST-KERNEL header is angle-bracketed on both substrates
# (<linux/netdevice.h>, <sys/mbuf.h>, <net/if.h>, <machine/...>, <uvm/...>), and
# cluster core code needs none of them: it gets its fixed-width types and its
# host primitives through exec_kbackend.h, which the BINDING resolves. So the
# rule is the simple one -- if it is in angle brackets and it is not one of the
# handful of C-standard headers below, it does not belong here.
#
# The allowlist exists because vms_cluster.h's OWN documented design (its
# "THIS HEADER DELIBERATELY DOES NOT INCLUDE exec_kbackend.h" paragraph) routes
# its host branch straight to <stdint.h> instead of vms_internal.h -- that is
# what makes the pure headers compile with a plain host compiler and NO kernel
# headers (design SS3.2.1/SS3.2.2, rung-1 of the test ladder). <stdint.h> is a
# FREESTANDING ISO C header available identically to every C compiler on every
# substrate (C99 7.18, "freestanding implementations" 4p6) -- it names no host
# kernel type and no substrate idiom, so flagging it is a false positive on the
# very design this gate exists to protect, not a leak. The allowlist is
# intentionally narrow: only headers with that same freestanding guarantee.
# ---------------------------------------------------------------------------
ANGLE_INCLUDE_ALLOWLIST='stdint\.h|stddef\.h|stdbool\.h|stdarg\.h|limits\.h|float\.h|iso646\.h'

check_angle_includes() {
	f="$1"
	strip_comments "$f" | grep -n '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' |
	while IFS=: read -r ln rest; do
		hdr=$(echo "$rest" | sed -n 's/.*<\([^>]*\)>.*/\1/p')
		if echo "$hdr" | grep -qE "^(${ANGLE_INCLUDE_ALLOWLIST})\$"; then
			continue
		fi
		echo "ANGLE|$ln|$(echo "$rest" | sed 's/^[[:space:]]*//')"
	done
}

# ---------------------------------------------------------------------------
# RULE 2 -- a quoted include must resolve inside src/kernel-core, or be
# vms_internal.h (the per-substrate struct twin every executive core TU includes;
# each substrate provides its own, which is the established core idiom).
# ---------------------------------------------------------------------------
check_quoted_includes() {
	f="$1"
	strip_comments "$f" | grep -n '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' |
	while IFS=: read -r ln rest; do
		hdr=$(echo "$rest" | sed -n 's/.*"\([^"]*\)".*/\1/p')
		[ -n "$hdr" ] || continue
		[ "$hdr" = "vms_internal.h" ] && continue
		if [ ! -f "$CORE/$hdr" ]; then
			echo "QUOTED|$ln|$hdr"
		fi
	done
}

# ---------------------------------------------------------------------------
# RULE 3 -- no substrate identifier in code.
#
# The leak table's own list, plus the two logging idioms the seam replaces
# (SS18 exec_console_printf exists precisely so cluster code stops calling
# pr_info/printk, a Linux spelling the NetBSD twin has to #define away).
# Comments are stripped first, so this fires only on real code.
# ---------------------------------------------------------------------------
SUBSTRATE_IDENTS='sk_buff|skb_|struct mbuf|m_gethdr|m_copyback|MGETHDR|net_device|struct ifnet|ifp->|netdev_|dev_queue_xmit|dev_add_pack|dev_remove_pack|dev_mc_add|dev_mc_del|if_transmit|if_mcast_op|ether_input|pfil_|softirq|softint|callout_|timer_list|mod_timer|del_timer|kthread_run|kthread_create|kthread_should_stop|spinlock_t|kmutex_t|kcondvar_t|IPL_[A-Z]|splnet|jiffies|ktime_|getnanotime|getnanouptime|GFP_|KM_SLEEP|KM_NOSLEEP|kmalloc|kzalloc|kfree|kmem_alloc|kmem_zalloc|kmem_free|copy_from_user|copy_to_user|copyin\(|copyout\(|printk|pr_info|pr_err|pr_warn|list_head|TAILQ_|LIST_HEAD|curlwp|current->'

check_substrate_idents() {
	f="$1"
	strip_comments "$f" | grep -nE "$SUBSTRATE_IDENTS" |
	while IFS=: read -r ln rest; do
		hit=$(echo "$rest" | grep -oE "$SUBSTRATE_IDENTS" | head -1)
		echo "IDENT|$ln|$hit"
	done
}

# ---------------------------------------------------------------------------
# RULE 4 -- a pure FSM or the codec touches NO seam primitive.
#
# Design SS3.9: an FSM reaches the world only through its injected `ops`, and
# reads the clock only through ops.now -- "so a test drives time". A codec is
# pure build/parse: no state, no allocation, no substrate call. Calling
# exec_lan_*/exec_timer_*/exec_kthread_*/exec_time_*/exec_console_printf or an
# allocator from one of those TUs is what makes it unrunnable on the host, which
# costs the rung-1 and rung-2 test ladders.
# ---------------------------------------------------------------------------
FSM_FORBIDDEN='exec_lan_|exec_kthread_|exec_timer_|exec_time_now_vms|exec_ticks_ms|exec_console_printf|exec_zalloc|exec_alloc|exec_free|exec_lock|exec_unlock|exec_cv_|exec_mutex_'

check_pure_tus() {
	f="$1"
	case "$f" in
	*_fsm.c|*_fsm.h|*vms_cluster_codec.c|*vms_cluster_codec.h) ;;
	*) return 0 ;;
	esac
	strip_comments "$f" | grep -nE "$FSM_FORBIDDEN" |
	while IFS=: read -r ln rest; do
		hit=$(echo "$rest" | grep -oE "$FSM_FORBIDDEN" | head -1)
		echo "PURE|$ln|$hit"
	done
}

scan_file() {
	f="$1"
	{
		check_angle_includes "$f"
		check_quoted_includes "$f"
		check_substrate_idents "$f"
		check_pure_tus "$f"
	} | while IFS='|' read -r rule ln detail; do
		case "$rule" in
		ANGLE)
			echo "$f:$ln: RULE1 substrate include in cluster core: $detail" ;;
		QUOTED)
			echo "$f:$ln: RULE2 include outside src/kernel-core: \"$detail\"" ;;
		IDENT)
			echo "$f:$ln: RULE3 substrate idiom in cluster core: $detail" ;;
		PURE)
			echo "$f:$ln: RULE4 pure FSM/codec TU calls a seam primitive: $detail" ;;
		esac
	done
}

# ---------------------------------------------------------------------------
# Negative control: the gate must REJECT an injected substrate include. Run in a
# throwaway tree so the repository is never modified.
# ---------------------------------------------------------------------------
self_test() {
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	mkdir -p "$tmp/src/kernel-core"
	cat > "$tmp/src/kernel-core/vms_pe.c" <<'EOF'
/* A deliberately broken vms_pe.c: the gate must reject every line below.
 * Note the substrate names inside THIS comment -- they must NOT trip the gate,
 * because a doc-comment explaining the seam is not a leak. mbuf sk_buff IPL_NET
 */
#include <linux/netdevice.h>
#include "../kernel/vms_ioctl.h"
static void rx(struct sk_buff *skb) { pr_info("leak"); }
EOF
	cat > "$tmp/src/kernel-core/vms_pe_fsm.c" <<'EOF'
#include "vms_pe.h"
static void t(void) { exec_timer_arm(0, 0); }
EOF
	: > "$tmp/src/kernel-core/vms_pe.h"

	out=$(OVMX_REPO="$tmp" "$0" 2>&1) && st=0 || st=$?
	echo "$out" | sed 's/^/    /'

	if [ "$st" -eq 0 ]; then
		echo "NEGATIVE CONTROL FAILED: the gate accepted an injected substrate include"
		return 1
	fi
	for want in RULE1 RULE2 RULE3 RULE4; do
		if ! echo "$out" | grep -q "$want"; then
			echo "NEGATIVE CONTROL FAILED: $want never fired on the injected file"
			return 1
		fi
	done
	# "mbuf" and "IPL_NET" appear ONLY inside the injected file's doc-comment.
	# If either shows up in a finding, strip_comments is not working and the
	# gate would redden every honest seam explanation in the tree.
	if echo "$out" | grep -qE 'RULE[0-9].*(mbuf|IPL_NET)'; then
		echo "NEGATIVE CONTROL FAILED: the gate fired on a substrate name inside a COMMENT"
		return 1
	fi
	echo "OK: the gate rejects an injected #include <linux/netdevice.h> (RULE1),"
	echo "    an out-of-core quoted include (RULE2), an sk_buff/pr_info in code (RULE3),"
	echo "    a seam call from a _fsm.c (RULE4), and does NOT fire on comment prose."
	return 0
}

if [ "${1:-}" = "--self-test" ]; then
	self_test
	exit $?
fi

files=$(cluster_files)
if [ -z "$files" ]; then
	echo "::error::cluster_core_includes_gate: no cluster core files found under $CORE"
	echo "a gate that scans nothing proves nothing"
	exit 2
fi

n=0
for f in $files; do
	n=$((n + 1))
	findings=$(scan_file "$f")
	if [ -n "$findings" ]; then
		echo "$findings" | while IFS= read -r line; do
			path=${line%%:*}
			rest=${line#*:}
			lineno=${rest%%:*}
			msg=${rest#*: }
			fail "$path" "$lineno" "${msg%% *}" "${msg#* }"
		done
		rc=1
	fi
done

if [ "$rc" -ne 0 ]; then
	echo "cluster core includes gate FAILED"
	echo "the cluster stack is written ONCE for both substrates: kernel-core"
	echo "cluster TUs include only exec_kbackend.h and kernel-core headers"
	echo "(docs/design-faithful-cluster-executive.md SS3.2.2)"
	exit 1
fi

echo "OK: $n kernel-core cluster files, no substrate include, no substrate idiom,"
echo "    no seam call from a pure FSM/codec TU"
exit 0
