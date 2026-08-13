#!/bin/sh
# gen_mmk_cld_src.sh (vms-ec70) — embed a .cld file as a C string literal.
# Usage: gen_mmk_cld_src.sh <in.cld> <out.h>
# Produces:  static const char mmk_cld_source[] = "....";
# (A shell script, not a *.cmake file, because .gitignore excludes *.cmake.)
set -e
IN=${1:?need input .cld}; OUT=${2:?need output .h}
{
    echo "/* generated from mmk_cld.cld by gen_mmk_cld_src.sh (vms-ec70) */"
    echo "static const char mmk_cld_source[] ="
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/  "/' -e 's/$/\\n"/' "$IN"
    echo ";"
} > "$OUT"
