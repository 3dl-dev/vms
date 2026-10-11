#!/bin/sh
# mk_vms_vector_shr.sh -- build an OVMX vector image (SYS$PUBLIC_VECTORS.EXE,
# LIBRTL.EXE) from its manifest (<NAME>.vec, this directory), rd vms-3b3f.
#
# The image's symbol vector puts each listed universal at the slot the real
# OpenVMS Alpha image has it (slot = byte offset / 16); every unlisted slot up
# to the highest listed one is a retired (PRIVATE_PROCEDURE) entry. Each listed
# slot is a null-frame procedure that jumps to the OVMX target with the
# caller's arguments, argument information and return address untouched, so
# the target runs as if called directly (the job a VMS vector image does).
# The linkage label is spelled $<n>..<target>..lk, the form the port compiler
# emits: the cross assembler only resolves "label+8" (the procedure-value half
# of the linkage pair) against a label of that form.
#
# Usage: mk_vms_vector_shr.sh <LINK.EXE> <manifest.vec> <out.EXE> <dir-of-use-images>[:<dir>...]
# Each '# use' image is taken from the first of the colon-separated directories
# that holds it.
# Env: ALPHA_CC (alpha-dec-vms-gcc).
set -e
LINK_EXE=${1:?usage: mk_vms_vector_shr.sh <LINK.EXE> <manifest.vec> <out.EXE> <usedir>}
VEC=${2:?need manifest}
OUT=${3:?need output}
USEDIR=${4:?need the directory holding the --use images}
: "${ALPHA_CC:?set ALPHA_CC=alpha-dec-vms-gcc}"

GSMATCH=$(sed -n 's/^#[[:space:]]*gsmatch[[:space:]]*//p' "$VEC")
USES=$(sed -n 's/^#[[:space:]]*use[[:space:]]*//p' "$VEC")
[ -n "$GSMATCH" ] || { echo "mk_vms_vector_shr: $VEC has no '# gsmatch' line" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
S="$WORK/vec.s"
: > "$S"
echo "	.set noreorder" >> "$S"
echo "	.set volatile" >> "$S"

# slot -> stub name, in a sorted list "slot name target"
grep -v '^[[:space:]]*#' "$VEC" | awk 'NF==3 {
    off = $1; sub(/^0[xX]/, "", off);
    v = 0; n = length(off);
    for (i = 1; i <= n; i++) v = v * 16 + index("0123456789abcdef", tolower(substr(off, i, 1))) - 1;
    if (v % 16) { print "BAD " $0; exit 1 }
    print v / 16, $2, $3
}' | sort -n > "$WORK/slots"
if grep -q '^BAD' "$WORK/slots"; then
    echo "mk_vms_vector_shr: offset not a multiple of 16: $(grep '^BAD' "$WORK/slots")" >&2; exit 2
fi
[ -s "$WORK/slots" ] || { echo "mk_vms_vector_shr: $VEC lists no entries" >&2; exit 2; }
if [ "$(cut -d' ' -f1 "$WORK/slots" | uniq -d)" ]; then
    echo "mk_vms_vector_shr: duplicate offset in $VEC" >&2; exit 2
fi

while read -r slot univ target; do
    stub="ovmx\$vec_$slot"
    cat >> "$S" <<ASM
	.text
	.align 4
	.globl $stub
	.ent $stub
$stub..en:
	.base \$27
	.frame \$30,0,\$26,0
	.prologue
	ldq \$1,\$$slot..$target..lk
	ldq \$27,\$$slot..$target..lk+8
	jmp \$31,(\$1)
	.link
	.align 3
$stub:
	.pdesc $stub..en,null
\$$slot..$target..lk:
	.linkage $target
	.end $stub
ASM
done < "$WORK/slots"

"$ALPHA_CC" -c -o "$WORK/vec.o" "$S"

# The ordered symbol vector: listed slots by stub, gaps retired.
MAX=$(tail -1 "$WORK/slots" | cut -d' ' -f1)
SV=""
i=0
while [ "$i" -le "$MAX" ]; do
    if grep -q "^$i " "$WORK/slots"; then
        e="ovmx\$vec_$i=PROCEDURE"
    else
        e="ovmx\$gap_$i=PRIVATE_PROCEDURE"
    fi
    SV="${SV:+$SV,}$e"
    i=$((i + 1))
done

USE_ARGS=""
for u in $USES; do
    f=""
    for d in $(echo "$USEDIR" | tr ':' ' '); do
        [ -f "$d/$u" ] && { f="$d/$u"; break; }
    done
    [ -n "$f" ] || { echo "mk_vms_vector_shr: --use image $u missing from $USEDIR" >&2; exit 2; }
    USE_ARGS="$USE_ARGS --use $f"
done
# shellcheck disable=SC2086
"$LINK_EXE" --shareable --symbol-vector "$SV" --gsmatch "$GSMATCH" $USE_ARGS \
    -o "$OUT" "$WORK/vec.o"
echo "mk_vms_vector_shr: $(basename "$OUT"): $((MAX + 1)) slots, $(wc -l < "$WORK/slots") implemented ($GSMATCH)"
