#!/bin/sh
#
# setup.sh -- build the Alpha lab node from nothing (rd vms-e2c).
#
# WHY AN ALPHA AT ALL. Lab-1 and lab-2 are OpenVMS VAX V7.3. VAX is a 32-bit
# architecture, so NO VAX NODE CAN ANSWER A QUADWORD QUESTION. Measuring OVMX
# against a VAX and concluding "VMS is 32-bit here" confuses the architecture
# with the OS, and that inference has already produced at least one change to
# OVMX (SHOW SYMBOL integer rendering, rd vms-c71). This node exists so 64-bit
# questions have a real oracle instead of an inference.
#
# WHAT THIS SCRIPT DOES NOT DO: install OpenVMS. That needs Alpha installation
# media and a licence, which are an operator decision -- see README.md. This
# script gets you a running AlphaServer ES40 sitting at its SRM console with an
# empty system disk, which is everything that does not depend on that decision.
#
# Everything lands under $LAB (default /data/training/vax/alpha) and NOTHING is
# installed on the host: AXPbox ships a prebuilt Linux amd64 binary whose shared
# library deps (libpcap, libX11, libstdc++) are already present on workshop.
#
# Usage: setup.sh [LAB_DIR]

set -eu

LAB="${1:-/data/training/vax/alpha}"
AXPBOX_VERSION="v1.2.0"
AXPBOX_URL="https://github.com/lenticularis39/axpbox/releases/download/${AXPBOX_VERSION}/AXPbox-linux-x86-gcc"

# The ES40 SRM console firmware. This is HP/DEC console firmware, used AS
# PUBLISHED and unmodified -- it is loaded and executed, never disassembled or
# decompiled. Project Rule 8 (clean-room VMS RE) forbids disassembling VSI/HPE
# material; running published console firmware to obtain an observation oracle
# is the same posture as running OpenVMS itself on the VAX lab nodes.
SRM_URL="http://raymii.org/s/inc/downloads/es40-srmon/cl67srmrom.exe"
SRM_MD5="2edbd7f28f17ef909cee32fd11632df7"

# 9G is comfortable for a full OpenVMS Alpha install plus layered products; the
# file is sparse, so it costs only what the guest actually writes.
SYSDISK_SIZE="9G"

say() { printf '\n=== %s\n' "$*"; }

say "lab root: $LAB"
mkdir -p "$LAB/rom" "$LAB/disks" "$LAB/cfg" "$LAB/logs" "$LAB/tools" "$LAB/media"

say "AXPbox ${AXPBOX_VERSION}"
if [ -x "$LAB/axpbox-${AXPBOX_VERSION#v}" ]; then
    echo "already present, skipping download"
else
    curl -fsSL -o "$LAB/axpbox-${AXPBOX_VERSION#v}" "$AXPBOX_URL"
    chmod +x "$LAB/axpbox-${AXPBOX_VERSION#v}"
fi
"$LAB/axpbox-${AXPBOX_VERSION#v}" 2>&1 | head -1

say "SRM console firmware"
if [ -f "$LAB/rom/cl67srmrom.exe" ]; then
    echo "already present, skipping download"
else
    curl -fsSL -o "$LAB/rom/cl67srmrom.exe" "$SRM_URL"
fi
got=$(md5sum "$LAB/rom/cl67srmrom.exe" | cut -d' ' -f1)
if [ "$got" != "$SRM_MD5" ]; then
    echo "FAIL: SRM ROM checksum $got != expected $SRM_MD5" >&2
    echo "  -> refusing to run an unverified console firmware image" >&2
    exit 1
fi
echo "md5 ok: $got"

say "system disk (${SYSDISK_SIZE}, sparse)"
if [ -f "$LAB/disks/alpha1-sys.img" ]; then
    echo "already present, NOT overwriting (it may hold an installed OpenVMS)"
else
    truncate -s "$SYSDISK_SIZE" "$LAB/disks/alpha1-sys.img"
fi
ls -la "$LAB/disks/alpha1-sys.img"

say "config + console driver"
here=$(cd "$(dirname "$0")" && pwd)
cp "$here/cfg/alpha1.cfg" "$LAB/cfg/"
cp "$here/tools/srmdrv.py" "$LAB/tools/"
chmod +x "$LAB/tools/srmdrv.py"

cat <<EOF

=== ready.

Start the machine (it waits for a console connection on TCP 21264):
    cd $LAB && ./axpbox-${AXPBOX_VERSION#v} run cfg/alpha1.cfg > logs/axpbox.log 2>&1 &

Attach to the SRM console:
    python3 $LAB/tools/srmdrv.py -t 180 -l $LAB/logs/console.log -w 'P00>>>'

Expected, in about 20 seconds:
    AlphaServer ES40 Console V7.3-1, built on Feb 27 2007 at 12:57:47
    P00>>>

Installing OpenVMS on it needs media -- see tests/lab-alpha/README.md.
EOF
