#!/bin/sh
set -e
apk add --no-cache gcc musl-dev binutils make linux-headers >/dev/null
CC=gcc
SRC=/src/src/libvms
LIBVMSSYS_DIR=/src/src/libvmssys
VMSPROC_INC=/src/src/vmsprocess/include
VMSLNM_INC=/src/src/vmslnm/include
VMSFS_INC=/src/src/vmsfs/include
VMSRMS_INC=/src/src/vmsrms/include
WORK=/tmp/measure
mkdir -p "$WORK"

CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics"
INCS="-I$SRC/include -I$LIBVMSSYS_DIR -I$VMSPROC_INC -I$VMSLNM_INC -I$VMSFS_INC -I$VMSRMS_INC"

LIST="descrip status \
syssvc/sys_assign syssvc/sys_mailbox syssvc/sys_qio syssvc/sys_uring syssvc/sys_event \
syssvc/sys_time syssvc/sys_process syssvc/sys_memory syssvc/sys_logical syssvc/sys_ast \
syssvc/sys_lock syssvc/sys_misc syssvc/sys_security syssvc/sys_fao syssvc/sys_msg \
syssvc/sys_float syssvc/sys_uai syssvc/sys_device syssvc/sys_operator syssvc/sys_condition \
rtl/ovmx_accounting rtl/lib_vm rtl/lib_output rtl/lib_signal rtl/lib_datetime rtl/lib_misc \
rtl/lib_dyndesc rtl/lib_logical rtl/lib_symbol rtl/lib_string_ops rtl/lib_bitops \
rtl/lib_eventflags rtl/str_routines rtl/mth_routines rtl/ots_routines rtl/sha256 \
rtl/sysuaf rtl/str_util"

OBJS=""
for c in $LIST; do
    b=$(echo "$c" | tr / _)
    $CC $CFLAGS $INCS -c -o "$WORK/$b.o" "$SRC/$c.c"
    OBJS="$OBJS $WORK/$b.o"
done

echo "=== all undefined vms_kif_* symbols referenced by libvms objects (nm -u) ==="
nm -u $OBJS 2>/dev/null | awk '{print $NF}' | grep '^vms_kif_' | sort -u

echo
echo "=== all undefined (non vms_kif_) symbols too, for completeness ==="
nm -u $OBJS 2>/dev/null | awk '{print $NF}' | sort -u | grep -v '^vms_kif_' | grep -v '^$'
