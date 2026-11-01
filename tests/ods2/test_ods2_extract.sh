#!/bin/sh
# rd vms-b85: `vmsfs_master --ods5 extract` copies a directory tree off a genuine
# ODS-5 (and ODS-2) volume byte for byte -- how a CI shard's GCC objects, written
# by an OVMX boot, reach the job that links them. Master a tree (mixed-case
# ODS-5 names, a binary .obj, a multi-block file, nested directories), extract a
# subtree, compare; a missing directory is an error, not an empty tree.
set -eu
M=${VMSFS_MASTER:?}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/src/SYS0/SYSCOMMON/GCCOBJ/BACKEND" "$T/src/SYS0/SYSCOMMON/GCCOBJ/Z" "$T/src/OTHER"
head -c 70000 /dev/urandom > "$T/src/SYS0/SYSCOMMON/GCCOBJ/BACKEND/tree-ssa.obj"
head -c 513 /dev/urandom > "$T/src/SYS0/SYSCOMMON/GCCOBJ/Z/libz_a-adler32.obj"
printf 'line one\nline two\n' > "$T/src/SYS0/SYSCOMMON/GCCOBJ/BUILD.LOG"
echo x > "$T/src/OTHER/NOT_ME.TXT"
fails=0
for lev in --ods5 --ods2; do
  rm -rf "$T/out" "$T/vol.img"
  "$M" $lev master "$T/vol.img" XTEST "$T/src" 8 > /dev/null
  "$M" $lev extract "$T/vol.img" "$T/out" SYS0.SYSCOMMON.GCCOBJ
  # ODS-2 names are upper case; compare case-blind there
  if [ "$lev" = --ods5 ]; then
    if diff -r "$T/src/SYS0/SYSCOMMON/GCCOBJ" "$T/out" > /dev/null; then echo "PASS $lev: subtree round-trips byte for byte"
    else echo "FAIL $lev: extracted tree differs"; diff -r "$T/src/SYS0/SYSCOMMON/GCCOBJ" "$T/out" | head; fails=$((fails+1)); fi
  else
    a=$(cd "$T/src/SYS0/SYSCOMMON/GCCOBJ" && find . -type f | sort | tr a-z A-Z | xargs -n1 basename | tr '\n' ' ')
    b=$(cd "$T/out" && find . -type f | sort | xargs -n1 basename | tr '\n' ' ')
    if [ "$a" = "$b" ] && cmp -s "$T/src/SYS0/SYSCOMMON/GCCOBJ/BACKEND/tree-ssa.obj" "$T/out/BACKEND/TREE-SSA.OBJ"; then
      echo "PASS $lev: subtree round-trips (upper-case names)"
    else echo "FAIL $lev: [$a] vs [$b]"; fails=$((fails+1)); fi
  fi
  [ ! -e "$T/out/NOT_ME.TXT" ] || { echo "FAIL $lev: extracted outside the subtree"; fails=$((fails+1)); }
  if "$M" $lev extract "$T/vol.img" "$T/out2" NO.SUCH.DIR 2>/dev/null; then
    echo "FAIL $lev: a missing directory extracted"; fails=$((fails+1))
  else echo "PASS $lev: a missing directory is an error"; fi
done
[ "$fails" -eq 0 ] && echo "test_ods2_extract: PASS" || { echo "test_ods2_extract: $fails FAILED"; exit 1; }
