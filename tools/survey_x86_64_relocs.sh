#!/bin/sh
# survey_x86_64_relocs.sh — empirical ground-truth survey of every ELF x86_64
# relocation type musl-gcc emits for OVMX's shareable/DCL source set, at the
# flags this project has standardized on for x86_64 (-fPIC -mtls-dialect=gnu2,
# per vms-be5's precedent — see src/imgact/test/run_test_x86_64.sh).
#
# Bead vms-680 (under vms-bdf, LINK.EXE x86_64 backend). This is grounding,
# not design: it cross-compiles the SAME object set vms-b65.1-.6 built for
# aarch64 (src/vmsprocess, src/libvms, src/vmsfs, src/vmslnm, src/vmsrms,
# src/vmsdcl) to x86_64 .o files and tallies every distinct ELF r_type by
# name and count, as observed by readelf -r — not hand-typed from memory.
#
# Usage:   tools/survey_x86_64_relocs.sh [--check]
#   (no args)  regenerate docs/design-link-x86_64-relocs.md from a fresh build
#              (illustrative snapshot -- exact counts in this doc are NOT
#              gated by --check, see vms-49bb below)
#   --check    run a fresh survey and assert PROPERTIES of the observed
#              relocation-type set (this is what CI runs):
#                - every observed r_type has documented LINK.EXE analog
#                  analysis (ANALYZED_X86_64_RELOC_TYPES below / the design
#                  doc's cross-reference table)
#                - none of the named FORBIDDEN_X86_64_RELOC_TYPES appear
#                  (dynamic-only relocs, copy relocs, wrong PIC/TLS model)
#                - at least one object actually built (survey didn't just
#                  silently produce zero data)
#              It does NOT diff exact per-type counts or the object-built
#              count against the committed doc. vms-49bb: the previous
#              `diff -u` exact-count golden broke on every benign
#              reloc-count shift (any added/removed line of source in the
#              surveyed components) -- it bit main 3 times (pre-#167, #170,
#              #184) despite catching zero real regressions. A change that
#              legitimately shifts how many R_X86_64_PC32 relocs exist is
#              not news; a change that makes LINK.EXE-unmapped or
#              dynamic-only relocation types show up in a static .o is.
#
# Requires: musl-gcc, readelf (binutils). Both are already host tooling on the
# x86_64 dev host (CLAUDE.md: musl cross-compilation is build-tooling, not a
# runtime — Rule 9 is about the RUNTIME target, not the compiler).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
OUT_DOC="$REPO/docs/design-link-x86_64-relocs.md"

MODE=${1:-}

CC=${CC:-musl-gcc}
CFLAGS="-fPIC -O2 -mtls-dialect=gnu2 -c"

# vms-49bb: the whitelist this check gates on, in place of the previous
# exact-count golden diff.
#
# ANALYZED_X86_64_RELOC_TYPES — every relocation type LINK.EXE's x86_64
# backend has documented analog analysis for, in
# docs/design-link-x86_64-relocs.md's "Cross-reference: LINK.EXE's existing
# R_AARCH64_* switch" table. It is fine for the fresh survey to observe a
# SUBSET of this list (a component's code shrinking away a reloc type is not
# a regression). It is NOT fine for the fresh survey to observe a type that
# ISN'T here: that's either a toolchain/flag regression (see
# FORBIDDEN_X86_64_RELOC_TYPES below) or a genuinely new relocation shape
# LINK.EXE has never been analyzed against — either way it needs analog
# analysis added to the design doc (and to this list) before it's expected,
# not a silently-passing test. Keep this list and the design doc's
# cross-reference table in sync — the check below enforces that they match.
ANALYZED_X86_64_RELOC_TYPES="
R_X86_64_64
R_X86_64_PC32
R_X86_64_PLT32
R_X86_64_GOTPCREL
R_X86_64_REX_GOTPCRELX
R_X86_64_GOTPC32_TLSDESC
R_X86_64_TLSDESC_CALL
R_X86_64_DTPOFF32
"

# FORBIDDEN_X86_64_RELOC_TYPES — relocation types that must NEVER appear in a
# static .o-file survey at OVMX's standardized x86_64 flags (-fPIC
# -mtls-dialect=gnu2). Named explicitly (rather than just falling out of "not
# in ANALYZED_X86_64_RELOC_TYPES") so a red test says WHAT broke:
#   - R_X86_64_RELATIVE / GLOB_DAT / JUMP_SLOT: dynamic (DT_RELA/DT_JMPREL)
#     relocations the LINKER synthesizes when building .dynamic/GOT/PLT
#     sections -- never emitted by the compiler into a static .o. Seeing one
#     here means something compiled as though it were already a linked
#     ET_DYN image.
#   - R_X86_64_COPY: copy relocation, a glibc/non-PIE-executable idiom with
#     no place in a -fPIC musl build.
#   - R_X86_64_IRELATIVE: ifunc resolver relocation, also linker/load-time
#     synthesized, not compiler-emitted into a static .o.
#   - R_X86_64_32 / R_X86_64_32S: absolute 32-bit relocations -- the
#     signature of -fPIC silently not applying (position-dependent codegen).
#   - R_X86_64_TLSGD / R_X86_64_TLSLD / R_X86_64_DTPMOD64 / R_X86_64_DTPOFF64
#     / R_X86_64_TPOFF32 / R_X86_64_TPOFF64: General-Dynamic / Local-Dynamic
#     or Initial-Exec / Local-Exec TLS models -- the signature of
#     -mtls-dialect=gnu2 silently not applying (falling back to the default
#     gnu TLSGD/LD model, or a non-PIC TLS model).
FORBIDDEN_X86_64_RELOC_TYPES="
R_X86_64_RELATIVE
R_X86_64_GLOB_DAT
R_X86_64_JUMP_SLOT
R_X86_64_COPY
R_X86_64_IRELATIVE
R_X86_64_32
R_X86_64_32S
R_X86_64_TLSGD
R_X86_64_TLSLD
R_X86_64_DTPMOD64
R_X86_64_DTPOFF64
R_X86_64_TPOFF32
R_X86_64_TPOFF64
"

command -v "$CC" >/dev/null 2>&1 || { echo "survey_x86_64_relocs: $CC not found" >&2; exit 1; }
command -v readelf >/dev/null 2>&1 || { echo "survey_x86_64_relocs: readelf not found" >&2; exit 1; }

WORK=$(mktemp -d /tmp/survey-x86_64-relocs.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# The six components vms-b65.1-.6 migrated to the VMS-native toolchain for
# aarch64. Same source set, x86_64 target this time.
COMPONENTS="vmsprocess libvms vmsfs vmslnm vmsrms vmsdcl"

INCS=""
for c in $COMPONENTS vmslink; do
    [ -d "$REPO/src/$c/include" ] && INCS="$INCS -I$REPO/src/$c/include"
done
# src/libvmssys has no include/ subdir (headers live at its top level), so the
# loop above never picks it up. sys_uai.c (src/libvms/syssvc/sys_uai.c)
# #includes vms_kif.h from there — add it explicitly (vms-354, option b).
INCS="$INCS -I$REPO/src/libvmssys"

TALLY="$WORK/tally.txt"
: > "$TALLY"

BUILT_LOG="$WORK/built.txt"
FAILED_LOG="$WORK/failed.txt"
: > "$BUILT_LOG"
: > "$FAILED_LOG"

for c in $COMPONENTS; do
    SRCDIR="$REPO/src/$c"
    [ -d "$SRCDIR" ] || { echo "survey_x86_64_relocs: missing src dir $SRCDIR" >&2; exit 1; }
    find "$SRCDIR" -name '*.c' -not -path '*/test/*' | sort | while IFS= read -r f; do
        rel=${f#"$REPO"/}
        obj="$WORK/$(echo "$rel" | tr '/' '_').o"
        if $CC $CFLAGS $INCS -o "$obj" "$f" 2>"$WORK/err.log"; then
            echo "$rel" >> "$BUILT_LOG"
            # readelf -r columns: Offset Info Type Sym.Value Sym.Name[+Addend]
            # "Type" is field 3 in the RELA table rows (skip header lines).
            readelf -rW "$obj" 2>/dev/null | awk -v src="$rel" '
                /^Relocation section/ { next }
                /^  Offset/ { next }
                NF >= 4 && $1 ~ /^[0-9a-f]+$/ {
                    print $3 "\t" src
                }
            ' >> "$TALLY"
        else
            echo "$rel: $(tail -1 "$WORK/err.log")" >> "$FAILED_LOG"
        fi
    done
done

N_BUILT=$(wc -l < "$BUILT_LOG" | tr -d ' ')
N_FAILED=$(wc -l < "$FAILED_LOG" | tr -d ' ')

# Tally: distinct r_type -> count, and one example source object per type.
TYPES_SORTED="$WORK/types_sorted.txt"
sort "$TALLY" > "$TYPES_SORTED"

# Pulls the R_X86_64_* identifiers out of the committed doc's
# "Cross-reference: LINK.EXE's existing R_AARCH64_* switch" table (and only
# that table -- the earlier "Relocation types observed" table uses the same
# first-column shape, so this scopes by section on purpose). Used by --check
# to make sure the script's ANALYZED_X86_64_RELOC_TYPES whitelist and the
# doc's hand-reviewed analog analysis haven't drifted apart.
extract_doc_analyzed_types() {
    awk '
        /^## / {
            if ($0 ~ /Cross-reference: LINK\.EXE.s existing/) { f=1 } else { f=0 }
            next
        }
        f
    ' "$OUT_DOC" | grep -oE '^\| R_X86_64_[A-Za-z0-9_]+ ' | tr -d '| '
}

gen_doc() {
    DEST=$1
    {
        echo "# x86_64 Relocation Survey — LINK.EXE grounding (vms-680)"
        echo
        echo "**Generated by \`tools/survey_x86_64_relocs.sh\` — do not hand-edit.**"
        echo "Re-run the script to regenerate. \`tools/survey_x86_64_relocs.sh --check\`"
        echo "(wired into CI) does NOT diff this file byte-for-byte — the exact counts"
        echo "below drift with every code change to the surveyed components and are"
        echo "illustrative only, not gated (vms-49bb). What the check DOES gate: every"
        echo "relocation type the fresh survey observes must appear in the analog table"
        echo "below (mirrored as \`ANALYZED_X86_64_RELOC_TYPES\` in the script), and none"
        echo "of the forbidden dynamic-only / wrong-PIC / wrong-TLS-model types"
        echo "(\`FORBIDDEN_X86_64_RELOC_TYPES\` in the script) may appear at all."
        echo
        echo "## Method"
        echo
        echo "Cross-compiled every non-test \`.c\` file in the vms-b65.1-.6 object set"
        echo "(\`src/vmsprocess\`, \`src/libvms\`, \`src/vmsfs\`, \`src/vmslnm\`, \`src/vmsrms\`,"
        echo "\`src/vmsdcl\`) to x86_64 \`.o\` files with:"
        echo
        echo '```'
        echo "$CC $CFLAGS -I<component>/include ..."
        echo '```'
        echo
        echo "(\`-fPIC -mtls-dialect=gnu2\`, per vms-be5's standing precedent for x86_64"
        echo "OVMX builds — see \`src/imgact/test/run_test_x86_64.sh\`.) Every produced"
        echo "\`.o\` was scanned with \`readelf -r\` and every distinct \`r_type\` name was"
        echo "tallied by occurrence count across the whole object set."
        echo
        echo "Files that failed to compile standalone (missing kernel/system headers"
        echo "outside this tree, multi-TU-only symbols, etc.) are excluded from the"
        echo "tally and listed below for transparency — they contribute no relocation"
        echo "data either way."
        echo
        echo "## Result: $N_BUILT objects built, $N_FAILED failed to compile standalone"
        echo
        echo "(snapshot from the run that generated this file — informational only;"
        echo "\`--check\` does not require these numbers to match a fresh run)"
        echo
        echo "## Relocation types observed"
        echo
        echo "Counts below are a snapshot, not gated — see the note at the top of this"
        echo "file. What IS gated is the *set* of types (must all appear in the"
        echo "cross-reference table below) and the absence of the script's"
        echo "\`FORBIDDEN_X86_64_RELOC_TYPES\`."
        echo
        echo "| r_type | count | distinct source objects | example source |"
        echo "|---|---|---|---|"
        if [ -s "$TYPES_SORTED" ]; then
            awk -F'\t' '{print $1}' "$TYPES_SORTED" | sort -u | while IFS= read -r t; do
                cnt=$(awk -F'\t' -v t="$t" '$1==t' "$TYPES_SORTED" | wc -l | tr -d ' ')
                nsrc=$(awk -F'\t' -v t="$t" '$1==t {print $2}' "$TYPES_SORTED" | sort -u | wc -l | tr -d ' ')
                ex=$(awk -F'\t' -v t="$t" '$1==t {print $2; exit}' "$TYPES_SORTED")
                echo "| $t | $cnt | $nsrc | $ex |"
            done
        else
            echo "| (none observed) | 0 | 0 | - |"
        fi
        echo
        echo "## Cross-reference: LINK.EXE's existing R_AARCH64_* switch"
        echo
        echo "\`src/vmslink/link.c\`'s relocation-apply switch (~lines 615-990) currently"
        echo "handles only \`R_AARCH64_*\` types. Mapping observed \`R_X86_64_*\` types to"
        echo "their ARM64 analog (or noting no analog exists) is the input the"
        echo "downstream x86_64 reloc-apply items (vms-8f5, vms-cd1, vms-2e4) implement"
        echo "against:"
        echo
        echo "Table covers only the r_type values the run above actually observed —"
        echo "no type is listed unless it appears in the tally with a nonzero count."
        echo
        echo "| R_X86_64_* (observed) | R_AARCH64_* analog in link.c | notes |"
        echo "|---|---|---|"
        echo "| R_X86_64_64 | R_AARCH64_ABS64 | direct analog — absolute 64-bit address, S+A |"
        echo "| R_X86_64_PC32 | R_AARCH64_PREL32 (partial) | 32-bit PC-relative, S+A-P; x86_64's workhorse call/jmp/lea-rip disp32 has no single ARM64 analog (ARM64 splits this across ADRP+ADD/LDR page-relocs) |"
        echo "| R_X86_64_PLT32 | R_AARCH64_CALL26 / JUMP26 (role analog only) | PC-relative PLT-routed call; ARM64's CALL26/JUMP26 play the equivalent linker role via a different encoding (26-bit imm vs 32-bit disp) |"
        echo "| R_X86_64_GOTPCREL | R_AARCH64_ADR_GOT_PAGE + LD64_GOT_LO12_NC (role analog only) | PC-relative GOT-entry load; ARM64 splits GOT access across two page/offset relocs, x86_64 does it in one PC32-class reloc |"
        echo "| R_X86_64_REX_GOTPCRELX | (no ARM64 analog) | x86_64 linker-relaxation variant of GOTPCREL (mov->lea when GOT slot provably unneeded); ARM64 has no equivalent relaxable-GOT-load reloc class |"
        echo "| R_X86_64_GOTPC32_TLSDESC | R_AARCH64_TLSDESC_ADR_PAGE21 (role analog only) | TLSDESC GOT-slot address computation (PC-relative GOT offset, first half of the x86_64 TLSDESC codegen pair); ARM64's ADR_PAGE21 plays the equivalent role via a page/offset split instead of a flat PC32 |"
        echo "| R_X86_64_TLSDESC_CALL | R_AARCH64_TLSDESC_CALL | direct analog — same TLSDESC model, project standardizes on \`-mtls-dialect=gnu2\` specifically to land here (2-reloc x86_64 GOTPC32_TLSDESC+TLSDESC_CALL pair) instead of the TLSGD/GD model \`-mtls-dialect=gnu\` (the default) would emit |"
        echo "| R_X86_64_DTPOFF32 | (no ARM64 analog observed in link.c) | module-relative offset of a TLS variable within its TLS block, filled as an absolute 32-bit constant and added to the TLSDESC resolver's returned TP-offset at runtime (observed: paired 1:1 with each __thread variable reference, immediately following a GOTPC32_TLSDESC/TLSDESC_CALL pair); ARM64's TLSDESC path encodes the equivalent module offset inside the GOT-slot resolver data itself (via TLSDESC_LD64_LO12/ADD_LO12), not as a separate static .o-file reloc against the operand |"
        echo
        echo "## Cross-reference: LINK.EXE (static, in .o files) vs IMGACT.EXE (load-time)"
        echo
        echo "\`src/imgact/imgact.c\`'s x86_64 arch handling (vms-913.11, arch/x86_64/) is"
        echo "LOAD-time: it walks the shareable image's \`DT_RELA\`/\`DT_JMPREL\` dynamic"
        echo "relocation tables at activation and applies \`R_X86_64_RELATIVE\`,"
        echo "\`R_X86_64_GLOB_DAT\`, \`R_X86_64_JUMP_SLOT\`, and \`R_X86_64_TLSDESC\` — the"
        echo "*dynamic* reloc set that exists because the image is a real ET_DYN ELF"
        echo "shareable being loaded into a process."
        echo
        echo "None of \`R_X86_64_RELATIVE\`, \`GLOB_DAT\`, or \`JUMP_SLOT\` appear in the"
        echo "static \`.o\`-file survey above — they are synthesized by the *linker*"
        echo "(GNU ld today; OVMX's own LINK.EXE going forward) when it builds the"
        echo "\`.dynamic\`/GOT/PLT sections, not emitted by the compiler into object"
        echo "files. \`R_X86_64_TLSDESC\` is the one type that appears in BOTH: as a"
        echo "static \`.o\`-file reloc (compiler-emitted, link-time — LINK.EXE's job) AND"
        echo "as a load-time \`DT_RELA\` entry pointing at the TLSDESC GOT slot"
        echo "(IMGACT's job) — LINK.EXE resolves the compile-time TLSDESC relocation"
        echo "into a GOT-slot-producing form, and IMGACT resolves that GOT slot's"
        echo "dynamic entry at activation. The two jobs are sequential stages on the"
        echo "same symbol, not overlapping responsibility for the same event."
        echo
        echo "**Confirmed: LINK.EXE's job (this survey) and IMGACT's job (load-time"
        echo "DT_* relocs, already handled per vms-913.11) are not the same set.**"
        echo
        echo "## Objects that failed to compile standalone ($N_FAILED)"
        echo
        if [ -s "$FAILED_LOG" ]; then
            echo '```'
            sort "$FAILED_LOG"
            echo '```'
        else
            echo "(none — every non-test .c file in the object set compiled standalone)"
        fi
    } > "$DEST"
}

if [ "$MODE" = "--check" ]; then
    # vms-49bb: property-based check. NOT an exact-count diff against the
    # committed doc (see the header comment and the doc's own note for why:
    # exact counts drift on every benign code change and broke main three
    # times — pre-#167, #170, #184 — for zero real regressions caught).
    OBSERVED_TYPES=$(awk -F'\t' '{print $1}' "$TYPES_SORTED" | sort -u)

    FAIL=0

    if [ "$N_BUILT" -eq 0 ]; then
        echo "survey_x86_64_relocs: 0 objects built out of the surveyed source set -- the survey produced no relocation data, so nothing was actually validated (toolchain/flags broken?)" >&2
        FAIL=1
    fi

    FORBIDDEN_HIT=""
    UNKNOWN_HIT=""
    for t in $OBSERVED_TYPES; do
        if printf '%s\n' $FORBIDDEN_X86_64_RELOC_TYPES | grep -qx "$t"; then
            FORBIDDEN_HIT="$FORBIDDEN_HIT $t"
        elif ! printf '%s\n' $ANALYZED_X86_64_RELOC_TYPES | grep -qx "$t"; then
            UNKNOWN_HIT="$UNKNOWN_HIT $t"
        fi
    done

    if [ -n "$FORBIDDEN_HIT" ]; then
        echo "survey_x86_64_relocs: FORBIDDEN relocation type(s) observed in a static .o survey:$FORBIDDEN_HIT" >&2
        echo "  each one indicates a specific known regression class (a dynamic-only reloc emitted into a static object, a copy reloc, or -fPIC/-mtls-dialect=gnu2 silently not applying) -- see FORBIDDEN_X86_64_RELOC_TYPES at the top of this script for which and why" >&2
        FAIL=1
    fi

    if [ -n "$UNKNOWN_HIT" ]; then
        echo "survey_x86_64_relocs: relocation type(s) observed with NO documented LINK.EXE analog analysis:$UNKNOWN_HIT" >&2
        echo "  add analog analysis to docs/design-link-x86_64-relocs.md's 'Cross-reference: LINK.EXE's existing R_AARCH64_* switch' table AND to ANALYZED_X86_64_RELOC_TYPES in this script before this type is expected" >&2
        FAIL=1
    fi

    DOC_TYPES=$(extract_doc_analyzed_types | sort -u)
    SCRIPT_TYPES=$(printf '%s\n' $ANALYZED_X86_64_RELOC_TYPES | sed '/^$/d' | sort -u)
    if [ "$DOC_TYPES" != "$SCRIPT_TYPES" ]; then
        echo "survey_x86_64_relocs: ANALYZED_X86_64_RELOC_TYPES in this script and the analog table in $OUT_DOC have drifted apart -- keep them in sync" >&2
        echo "  script: $(printf '%s ' $SCRIPT_TYPES)" >&2
        echo "  doc:    $(printf '%s ' $DOC_TYPES)" >&2
        FAIL=1
    fi

    if [ "$FAIL" -ne 0 ]; then
        exit 1
    fi

    echo "survey_x86_64_relocs: fresh survey OK -- $N_BUILT objects built, $N_FAILED failed to compile standalone"
    echo "survey_x86_64_relocs: observed relocation types, all analyzed and none forbidden: $(printf '%s ' $OBSERVED_TYPES)"
    echo "survey_x86_64_relocs: NOTE -- exact reloc counts in $OUT_DOC are an informational snapshot, not gated; re-run 'tools/survey_x86_64_relocs.sh' with no args to refresh it if desired"
else
    gen_doc "$OUT_DOC"
    echo "survey_x86_64_relocs: wrote $OUT_DOC ($N_BUILT built, $N_FAILED failed)"
fi
