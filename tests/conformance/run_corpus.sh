#!/bin/bash
#
# run_corpus.sh - Corpus-Driven Conformance Harness
#
# Compiles each .c file in tests/corpus/tier1-examples/ against OVMX headers,
# categorizes results, and outputs a machine-readable JSON report to stdout.
#
# Categories:
#   compile-pass  - compiled successfully
#   compile-fail  - gcc returned non-zero during compilation
#   link-fail     - compiled but linker failed (detected via error patterns)
#   run-pass      - linked and exited 0
#   run-fail      - linked and exited non-zero
#   run-crash     - linked and terminated by signal (exit code > 128)
#
# Regression detection: if BASELINE_FILE exists, any program that previously
# passed (compile-pass, run-pass, run-fail) but now fails compile is flagged.
#
# Usage:
#   run_corpus.sh [BASELINE_FILE]
#
# BASELINE_FILE defaults to the corpus_baseline.json in the same directory.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Resolve corpus directory relative to this script's location.
# The script lives at tests/conformance/run_corpus.sh, so the corpus is
# two levels up then into tests/corpus/tier1-examples.
# Inside Docker the repo is mounted at /src, so the same relative path works.
_candidate="${SCRIPT_DIR}/../corpus/tier1-examples"
if [ -d "${_candidate}" ]; then
    CORPUS_DIR="$(cd "${_candidate}" && pwd)"
elif [ -d "/src/tests/corpus/tier1-examples" ]; then
    CORPUS_DIR="/src/tests/corpus/tier1-examples"
else
    echo "ERROR: corpus directory not found (tried ${_candidate} and /src/tests/corpus/tier1-examples)" >&2
    exit 1
fi
unset _candidate

BASELINE_FILE="${1:-${SCRIPT_DIR}/corpus_baseline.json}"
BUILD_DIR="/tmp/corpus_conformance_build"
TIMESTAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# Compilation flags — mirrors run_conformance.sh and the builder stage paths
INCLUDE_FLAGS="-I/src/src/libvms/include -I/src/src/vmsprocess/include -I/src/src/vmsfs/include -I/src/src/vmsrms/include"
LIB_FLAGS="-L/src/build/lib -lvmsrms -lvmsfs -lvms -lvmsprocess -lvmssys -lpthread -lm"
CFLAGS="-O2"

# errchk.h is in the tier1-examples directory itself
INCLUDE_FLAGS="${INCLUDE_FLAGS} -I${CORPUS_DIR}"

mkdir -p "${BUILD_DIR}"

# ---------------------------------------------------------------------------
# Accumulator arrays (bash associative arrays require bash 4+)
# ---------------------------------------------------------------------------
declare -A prog_status
declare -A prog_errors
declare -A prog_missing_symbols
declare -A prog_missing_headers

declare -a all_missing_functions
declare -a all_missing_headers
declare -a all_missing_constants

count_compile_pass=0
count_compile_fail=0
count_link_fail=0
count_run_pass=0
count_run_fail=0
count_run_crash=0
total=0

# ---------------------------------------------------------------------------
# Load baseline for regression detection
# ---------------------------------------------------------------------------
declare -A baseline_status
if [ -f "${BASELINE_FILE}" ] && command -v jq >/dev/null 2>&1; then
    while IFS= read -r line; do
        name="${line%%:*}"
        status="${line##*:}"
        baseline_status["${name}"]="${status}"
    done < <(jq -r '.programs[] | "\(.name):\(.status)"' "${BASELINE_FILE}" 2>/dev/null || true)
fi

# ---------------------------------------------------------------------------
# Helper: JSON-escape a string (handles backslash, quote, control chars)
# ---------------------------------------------------------------------------
json_escape() {
    local s="$1"
    # Escape backslash first, then double-quote, then strip control characters
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    printf '%s' "${s}"
}

# ---------------------------------------------------------------------------
# Helper: append a value to a newline-separated accumulator variable
# ---------------------------------------------------------------------------
append_unique() {
    local -n _arr="$1"
    local val="$2"
    local item
    for item in "${_arr[@]+"${_arr[@]}"}"; do
        [ "${item}" = "${val}" ] && return
    done
    _arr+=("${val}")
}

# ---------------------------------------------------------------------------
# Process each corpus program
# ---------------------------------------------------------------------------
for src_file in "${CORPUS_DIR}"/*.c; do
    [ -f "${src_file}" ] || continue

    name="$(basename "${src_file}" .c)"
    rel_file="tier1-examples/$(basename "${src_file}")"
    bin="${BUILD_DIR}/${name}"
    compile_log="${BUILD_DIR}/${name}.compile.log"

    total=$((total + 1))

    # -- Compile -------------------------------------------------------------
    compile_errors=""
    missing_syms=""
    missing_hdrs=""
    missing_consts=""

    gcc_rc=0
    gcc ${CFLAGS} ${INCLUDE_FLAGS} -o "${bin}" "${src_file}" ${LIB_FLAGS} \
        >"${compile_log}" 2>&1 || gcc_rc=$?

    if [ ${gcc_rc} -ne 0 ]; then
        # Distinguish link failures from source-level compile failures.
        # Source errors: "<file>.c:<line>:<col>: error:" pattern (from gcc front-end)
        # Link errors:   "undefined reference to" or "ld: ..." (from the linker)
        has_source_error=false
        has_link_error=false
        while IFS= read -r errline; do
            if [[ "${errline}" =~ \.c:[0-9]+:[0-9]+:\ error: ]]; then
                has_source_error=true
            fi
            if [[ "${errline}" =~ "undefined reference to" ]]; then
                has_link_error=true
            fi
        done < "${compile_log}"

        if ${has_link_error} && ! ${has_source_error}; then
            status="link-fail"
            count_link_fail=$((count_link_fail + 1))
        else
            status="compile-fail"
            count_compile_fail=$((count_compile_fail + 1))
        fi

        # Extract errors (first 20 lines to keep JSON manageable)
        compile_errors="$(head -20 "${compile_log}")"

        # Extract missing symbols: "undefined reference to `xxx`"
        while IFS= read -r sym; do
            sym="${sym// /}"
            [ -n "${sym}" ] && append_unique all_missing_functions "${sym}"
            missing_syms="${missing_syms}${sym}"$'\n'
        done < <(grep -oP "(?<=undefined reference to \`)[^\`]+" "${compile_log}" 2>/dev/null | sort -u || true)

        # Extract missing symbols: "implicit declaration of function 'xxx'"
        while IFS= read -r sym; do
            sym="${sym// /}"
            [ -n "${sym}" ] && append_unique all_missing_functions "${sym}"
            missing_syms="${missing_syms}${sym}"$'\n'
        done < <(grep -oP "(?<=implicit declaration of function ')[^']+" "${compile_log}" 2>/dev/null | sort -u || true)

        # Extract missing headers: "fatal error: xxx.h: No such file"
        while IFS= read -r hdr; do
            [ -n "${hdr}" ] && append_unique all_missing_headers "${hdr}"
            missing_hdrs="${missing_hdrs}${hdr}"$'\n'
        done < <(grep -oP "(?<=fatal error: )\S+(?=: No such file)" "${compile_log}" 2>/dev/null | sort -u || true)

        # Extract missing constants: "error: 'XXX' undeclared"
        while IFS= read -r const; do
            [ -n "${const}" ] && append_unique all_missing_constants "${const}"
            missing_consts="${missing_consts}${const}"$'\n'
        done < <(grep -oP "(?<=error: ')[^']+(?=' undeclared)" "${compile_log}" 2>/dev/null | sort -u || true)

        prog_status["${name}"]="${status}"
        prog_errors["${name}"]="${compile_errors}"
        prog_missing_symbols["${name}"]="${missing_syms}"
        prog_missing_headers["${name}"]="${missing_hdrs}"
        continue
    fi

    # -- Run -----------------------------------------------------------------
    run_log="${BUILD_DIR}/${name}.run.log"
    run_rc=0
    LD_LIBRARY_PATH=/src/build/lib timeout 10 "${bin}" >"${run_log}" 2>&1 || run_rc=$?

    if [ ${run_rc} -eq 0 ]; then
        status="run-pass"
        count_run_pass=$((count_run_pass + 1))
    elif [ ${run_rc} -gt 128 ]; then
        status="run-crash"
        count_run_crash=$((count_run_crash + 1))
    else
        status="run-fail"
        count_run_fail=$((count_run_fail + 1))
    fi

    prog_status["${name}"]="${status}"
    prog_errors["${name}"]=""
    prog_missing_symbols["${name}"]=""
    prog_missing_headers["${name}"]=""
done

# ---------------------------------------------------------------------------
# Regression detection
# ---------------------------------------------------------------------------
declare -a regressions

passing_statuses=("compile-pass" "run-pass" "run-fail")

for name in "${!baseline_status[@]}"; do
    bl="${baseline_status[${name}]}"
    cur="${prog_status[${name}]:-unknown}"

    # Was passing before?
    was_passing=false
    for ps in "${passing_statuses[@]}"; do
        [ "${bl}" = "${ps}" ] && was_passing=true && break
    done

    # Is now failing compile or link?
    now_failing=false
    case "${cur}" in
        compile-fail|link-fail|unknown) now_failing=true ;;
    esac

    if ${was_passing} && ${now_failing}; then
        regressions+=("${name}")
    fi
done

# ---------------------------------------------------------------------------
# Emit JSON to stdout
# ---------------------------------------------------------------------------

# Helper: emit a JSON string array from a bash array
emit_json_array() {
    local -n _items="$1"
    local first=true
    printf '['
    for item in "${_items[@]+"${_items[@]}"}"; do
        [ -z "${item}" ] && continue
        ${first} || printf ','
        printf '"%s"' "$(json_escape "${item}")"
        first=false
    done
    printf ']'
}

# Helper: emit errors array (split on newlines)
emit_errors_array() {
    local raw="$1"
    local first=true
    printf '['
    while IFS= read -r line; do
        [ -z "${line}" ] && continue
        ${first} || printf ','
        printf '"%s"' "$(json_escape "${line}")"
        first=false
    done <<< "${raw}"
    printf ']'
}

printf '{\n'
printf '  "timestamp": "%s",\n' "${TIMESTAMP}"
printf '  "total": %d,\n' "${total}"
printf '  "summary": {\n'
printf '    "compile-pass": %d,\n' "${count_compile_pass}"
printf '    "compile-fail": %d,\n' "${count_compile_fail}"
printf '    "link-fail": %d,\n' "${count_link_fail}"
printf '    "run-pass": %d,\n' "${count_run_pass}"
printf '    "run-fail": %d,\n' "${count_run_fail}"
printf '    "run-crash": %d\n' "${count_run_crash}"
printf '  },\n'
printf '  "programs": [\n'

first_prog=true
for src_file in "${CORPUS_DIR}"/*.c; do
    [ -f "${src_file}" ] || continue
    name="$(basename "${src_file}" .c)"
    rel_file="tier1-examples/$(basename "${src_file}")"
    status="${prog_status[${name}]:-unknown}"

    ${first_prog} || printf ',\n'
    first_prog=false

    # Build missing_symbols array from newline-separated string
    declare -a sym_arr=()
    while IFS= read -r sym; do
        [ -n "${sym}" ] && sym_arr+=("${sym}")
    done <<< "${prog_missing_symbols[${name}]:-}"

    declare -a hdr_arr=()
    while IFS= read -r hdr; do
        [ -n "${hdr}" ] && hdr_arr+=("${hdr}")
    done <<< "${prog_missing_headers[${name}]:-}"

    printf '    {\n'
    printf '      "name": "%s",\n' "$(json_escape "${name}")"
    printf '      "file": "%s",\n' "$(json_escape "${rel_file}")"
    printf '      "status": "%s",\n' "$(json_escape "${status}")"
    printf '      "errors": '
    emit_errors_array "${prog_errors[${name}]:-}"
    printf ',\n'
    printf '      "missing_symbols": '
    emit_json_array sym_arr
    printf ',\n'
    printf '      "missing_headers": '
    emit_json_array hdr_arr
    printf '\n'
    printf '    }'

    unset sym_arr hdr_arr
done

printf '\n  ],\n'

# gaps
printf '  "gaps": {\n'
printf '    "missing_functions": '
emit_json_array all_missing_functions
printf ',\n'
printf '    "missing_headers": '
emit_json_array all_missing_headers
printf ',\n'
printf '    "missing_constants": '
emit_json_array all_missing_constants
printf '\n'
printf '  },\n'

# regressions
printf '  "regressions": '
emit_json_array regressions
printf '\n'

printf '}\n'

# ---------------------------------------------------------------------------
# Exit code: non-zero if regressions detected
# ---------------------------------------------------------------------------
if [ ${#regressions[@]} -gt 0 ]; then
    printf '\n[REGRESSION] %d regression(s) detected: %s\n' \
        "${#regressions[@]}" "${regressions[*]}" >&2
    exit 1
fi

exit 0
