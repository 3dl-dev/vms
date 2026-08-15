#!/bin/bash
# negctl_gate.sh - bash mirror of vaxharness.py's negctl_gate(), so a
# run-*.sh wrapper and its Python driver can never disagree about what
# "negative control satisfied" means (rd vms-cf5, parent vms-8e8).
#
# THE BUG THIS KILLS: run-eflag.sh's negctl-load mode expects the driver
# SESSION to exit NONZERO for "negctl satisfied" (see its
# `if run_session prove skip; then die "NO TEETH"; fi` shape); a
# since-fixed driver variant instead exited 0 and logged "negctl ok" --
# driver and wrapper silently disagreed about the contract. This file gives
# every run-*.sh wrapper ONE function to apply the inversion instead of each
# wrapper hand-rolling its own `if ...; then die ...; fi` around a driver
# invocation, which is exactly how the three variants in this repo diverged.
#
# CONTRACT (identical to vaxharness.py's negctl_gate() docstring -- keep
# both in sync if this ever changes):
#   - A driver's own exit code is NEVER mode-aware: 0 = every positive
#     assertion held, nonzero = at least one failed, REGARDLESS of negctl
#     mode. Drivers must not special-case negctl into an early exit 0.
#   - THIS function is what inverts the meaning for negctl mode:
#       positive mode (negctl=0): gate satisfied <=> driver exit code == 0
#       negctl mode   (negctl=1): gate satisfied <=> driver exit code != 0
#
# Usage (source this file, then):
#   run_driver ...; rc=$?
#   if vaxharness_negctl_gate "$rc" 0; then   # positive mode
#     echo PASS
#   else
#     echo FAIL
#   fi
#
#   run_driver ... ; rc=$?
#   if vaxharness_negctl_gate "$rc" 1; then   # negctl mode
#     echo "PASS: negative control has teeth"
#   else
#     echo "FAIL: negative control did NOT fail -- no teeth"
#   fi
#
# This is OVMX's own code (CLAUDE.md Rule 8): no third-party source.

# vaxharness_negctl_gate DRIVER_EXIT_CODE NEGCTL_FLAG
#   DRIVER_EXIT_CODE: the raw exit status of the driver invocation (an
#     integer; $? from the caller).
#   NEGCTL_FLAG: "1"/"true" for negctl mode, "0"/"false"/"" for positive
#     mode.
# Returns (via $?) 0 if the gate is satisfied, 1 otherwise -- so it is
# directly usable as an `if` condition.
vaxharness_negctl_gate() {
  local driver_exit_code="$1"
  local negctl_flag="${2:-0}"

  case "${negctl_flag}" in
    1 | true | TRUE | yes | YES) negctl_flag=1 ;;
    *) negctl_flag=0 ;;
  esac

  if [ "${negctl_flag}" -eq 1 ]; then
    # negctl mode: satisfied iff the driver did NOT exit 0.
    [ "${driver_exit_code}" -ne 0 ]
  else
    # positive mode: satisfied iff the driver exited exactly 0.
    [ "${driver_exit_code}" -eq 0 ]
  fi
}
