#!/bin/sh
# scsd_wire_diff.sh - byte-diff every frame two revisions of scsd.c transmit
# (vms-561).
#
#   usage: tools/cluster/scsd_wire_diff.sh <before-tree> [<after-tree>]
#
# <before-tree> and <after-tree> are checkouts of this repository (e.g.
#   git archive origin/work/vms-187-closure | tar -x -C /tmp/before
# ). <after-tree> defaults to the tree this script lives in.
#
# It compiles tools/cluster/scsd_wire_dump.c -- which #includes the tree's
# scsd.c with the transmit primitive macro-renamed -- once per tree, runs both
# over the same replay script, and diffs the hex dumps. Exit 0 means every frame
# is byte-identical; any diff is printed.
#
# THE DUMP TOOL IS TAKEN FROM THE *AFTER* TREE AND USED FOR BOTH BUILDS. That is
# deliberate: the "before" tree predates this item and has no such tool, and a
# harness that had to be committed to the before tree could not measure it. The
# tool patches nothing -- it only includes scsd.c -- so using one copy against
# two sources is what makes the comparison a comparison of the daemon.
set -eu

here=$(cd "$(dirname "$0")/../.." && pwd)
before=${1:?usage: scsd_wire_diff.sh <before-tree> [<after-tree>]}
after=${2:-$here}
tool=$here/tools/cluster/scsd_wire_dump.c
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

build() {
    tree=$1
    tag=$2
    cc ${CFLAGS:--O0 -g} -std=gnu11 -o "$work/dump-$tag" "$tool" \
        -DSCSD_SOURCE="\"$tree/src/vmsscs/scsd.c\"" \
        -I"$tree/src/vmsscs/include" -I"$tree/src/libvms/include" \
        "$tree"/src/vmsscs/scs_classify.c "$tree"/src/vmsscs/scs_hello.c \
        "$tree"/src/vmsscs/scs_connect.c "$tree"/src/vmsscs/scs_dir.c \
        "$tree"/src/vmsscs/scs_start.c "$tree"/src/vmsscs/scs_vc.c \
        "$tree"/src/vmsscs/scs_member.c "$tree"/src/vmsscs/scs_config.c \
        "$tree"/src/vmsscs/scs_cdt.c "$tree"/src/vmsscs/scs_conn.c \
        "$tree"/src/vmsscs/scs_credit.c "$tree"/src/vmsscs/scs_depart.c \
        "$tree"/src/vmsscs/scs_dgram.c \
        $( [ -f "$tree/src/vmsscs/scs_svc.c" ] && echo "$tree/src/vmsscs/scs_svc.c" )
    "$work/dump-$tag" "$work/frames-$tag.txt"
}

build "$before" before
build "$after" after

if diff -u "$work/frames-before.txt" "$work/frames-after.txt"; then
    printf 'WIRE UNCHANGED: %s frame line(s) identical\n' \
        "$(wc -l < "$work/frames-after.txt")"
    grep -c '^FRAME' "$work/frames-after.txt" | sed 's/^/frames compared: /'
    exit 0
fi
echo "WIRE CHANGED -- see the diff above" >&2
exit 1
