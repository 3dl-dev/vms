#!/bin/bash
# TEST: SHOW USERS/FULL is the per-process form, distinct from bare SHOW USERS
#
# THE FIDELITY BUG THIS PINS: an earlier cmd_show_users() printed the same
# per-process table regardless of /FULL, so `SHOW USERS` and `SHOW USERS/FULL`
# rendered identically. Real VMS (VSI OpenVMS DCL Dictionary, SHOW USERS --
# https://www0.mi.infn.it/~calcolo/OpenVMS/ssb71/9996/9996p060.htm) prints two
# DIFFERENT tables: the default is per-user counts (Username Node Interactive
# Subprocess Batch); /FULL is per-process rows (Username Node Process Name PID
# Terminal, plus port information OVMX has none of).
#
# This asserts the /FULL form: its header carries the per-process "Process
# Name" and "Terminal" columns and does NOT carry the default form's
# "Interactive" count column. test_show_users_terminal.sh asserts the mirror
# image for the default form; together they prove the two forms differ.
#
# HONESTY INVARIANT (shared with the default-form test): ctest runs with no
# /dev/vms (Rule 9 -- the only runtime is the kernel/QEMU path), so there is
# no executive process table to read and no terminal-bound row to list. The
# /FULL table therefore prints its HEADING (a static string) but NO ROWS: no
# fabricated PID (regex:[0-9A-F]{8}), no fabricated terminal device name
# (regex:_[A-Z]{2,3}[0-9]+:). The positive proof -- /FULL naming a real
# logged-in session by its real executive-assigned VMS PID -- lives in
# tests/uat/vms_session_qemu.sh against a real executive.
#
# Trademark ceiling (INV-0): the banner brands OVMX, never OpenVMS.
#
# EXPECT: contains:Process Name
# EXPECT: contains:Terminal
# EXPECT: contains:OpenVMX
# EXPECT_NOT: contains:Interactive
# EXPECT_NOT: contains:OpenVMS
# EXPECT_NOT: regex:_[A-Z]{2,3}[0-9]+:
# EXPECT_NOT: regex:[0-9A-F]{8}
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'SHOW USERS/FULL' | $VMSDCL 2>&1
