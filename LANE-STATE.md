# LANE-STATE — vms-164d0 dead-code mop-up (wave 1)

Lane: SEAT A, DEAD-CODE MOP-UP. Scope: src/**, tools/**, build files, tests/** (no docs).

## Baseline (before any change)
- `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON` — configures clean.
- `cmake --build build -j$(nproc)` — EXIT=0, no errors. 544 pre-existing warning lines (format-truncation,
  incompatible-pointer-type, etc.) — none of them are dead-code signals except the two below; left alone
  (not this lane's scope, they're semantic/portability warnings, not unreferenced code).
- `ctest` (host suite, 277 tests) — **100% passed, 0 failed**, 584s. This is the true baseline; re-run
  after my two changes (full rebuild + full ctest) reproduced the same result (see below).
- Note: `tests/qemu/*` (the executive/kernel-module proof suite, e.g. test_syssvc_loginout_acp) is
  QEMU-guest tooling gated behind the real /dev/vms — it builds cleanly on this host but is not runnable
  by local `ctest`; it needs the heavy multi-arch CI. I verified those files still *compile* after my
  edits (targeted `cmake --build build --target <name>`), but CI is the acceptance signal for that tier.

## DONE (removed, with evidence)

1. **`tests/qemu/test_syssvc_loginout_acp.c`: removed unused `static void le64w(...)`.**
   Evidence: GCC `-Wunused-function` flagged it (`'le64w' defined but not used`) in the full build log.
   Grepped the file: `le64w` had exactly one definition and zero call sites (its sibling `le32w` IS
   called, at line ~182, and was kept). Rebuilt the single target after removal — compiles clean, warning
   gone, no other reference anywhere in the tree.

2. **`tools/gen_ssdef.py` — deleted (`git rm`).**
   Evidence:
   - Whole-repo search (`rg -l --hidden --fixed-strings gen_ssdef.py -g '!.git/**' .`) — zero hits anywhere,
     including `.github/workflows/*.yml`, all CMakeLists/Makefiles, all docs, all scripts.
   - Its own docstring claims it's "the canonical source for all SS$ status codes" that generates
     `src/libvms/include/ssdef.h` — but that header's own comment block says values are hand-matched
     to the real OpenVMS System Services Reference Manual, with **no mention of being generated**, and
     its documented bit layout ("Bits 0-2: Severity ... Bits 16-27: Facility ... Bits 29-31: Reserved")
     does not match the script's docstring layout ("Bits 31-28: Control ... Bits 2-0: Severity"). The
     claimed generator and its claimed target have drifted apart — a second, abandoned hand-copy of a
     concept the checked-in header doesn't source from.
   - `git log --follow` shows it was added incidentally inside an unrelated PR (`aa1d30fa`, "SHOW MEMORY
     authentic multi-section report") and never touched again — no adoption commit, no wiring commit.
   - Confirms this project's own single-source-of-truth policy (never hand-edit a generated file / never
     keep a second hand-maintained copy) — this script was the orphaned generator half of a pair that
     never got wired up.

LOC delta: -1 line (le64w) + -194 lines (gen_ssdef.py) = **-195 lines**, 0 lines added, 1 file removed,
1 file edited.

## IN-FLIGHT
None — both changes above are committed (see commits on this branch) and the branch is otherwise clean.

## REMAINING CANDIDATES (checked, NOT removed — keep or hand to successor for a second look)

- **`tools/cross-alpha/build-libstack-alpha.sh`** — also zero references anywhere in the tree (incl.
  hidden CI dirs and docs). *Not* removed: unlike gen_ssdef.py, this is a manual operator proof script
  ("Rung A3 of the OVMX-on-Alpha epic", rd vms-fed) of a kind other sibling scripts in the same directory
  clearly are (some cross-alpha/*.sh ARE referenced from CI, this one isn't but reads like a deliberately
  manual dev tool, not an abandoned generator with a diverged target). Evidence for "dead" is weaker here
  (absence-only, no contradiction/drift signal like gen_ssdef.py had). Recommend: ask Baron/conductor
  whether Alpha-lane manual tooling like this is still wanted before deleting; if the Alpha lane operator
  confirms it's stale, it's a clean `git rm`.
- **`src/vmsssh/{ssh_ident,cred_drop,term_map}.{c,h}`** — looked like SSH-scaffold LARP candidates per the
  dead-code brief, but are explicitly documented as intentionally-kept substrate TUs (vms-97d/vms-49e/
  vms-6ae) that the real upstream-OpenSSH port (vms-9ef) will bind against; each has a real, passing unit
  test (`tests/vmsssh/CMakeLists.txt`) that link the actual product TU, not a mock. The vmssshd DAEMON
  and its libssh/wrap veneer were ALREADY fully retired in commit `61998c0a` (vms-d916, 6 commits before
  this lane started) — 4298 lines removed then. Nothing left to remove here; do not re-touch.
- **`SCSD.EXE` / userspace SCS strawman** — already fully deleted (FC-P3.9); confirmed via CMakeLists.txt
  comment, no source remains.
- Full orphan sweep performed and came back clean (false positives ruled out, not removed):
  - Every `.h` under `src/**`: zero orphans (all included somewhere).
  - Every `.c` under `src/**`: ~25 initial "no CMakeLists match" hits were ALL false positives — referenced
    via `.o` names in `src/kernel/Makefile` (two-object-list trap, vms_bg_datafd.c/vms_bg_forkinherit.c),
    via `mk_*.sh` custom build scripts (vmslink/vmsrms family), or via musl-arch cross toolchain scripts.
  - Every `.c` under `tests/**`: the ~200 "no literal match" hits are because `tests/qemu/CMakeLists.txt`
    uses `file(GLOB ... test_syssvc_*.c)` / similar globs, not literal names — all are live. `tests/corpus/**`
    is vendored third-party test corpus (tcc/mmk/regex/memtester etc.) built by its own harness, not CMake —
    out of scope, do not touch without checking that harness first.
  - `tools/**` scripts: `tools/ledger/*.py` and `tools/cross-alpha-vms/ots/run_ots_proof.sh` looked orphaned
    under a non-hidden-dir search but ARE referenced from `.github/workflows/{ledger,ci}.yml` — remember
    to search with `rg --hidden` (workflows live under `.github/`, a dot-dir ripgrep skips by default).

## NEXT ACTIONS for a successor
1. Re-run the orphan sweeps above periodically as new files land — the methodology (rg --hidden fixed-string
   basename search across the WHOLE tree, cross-checked against build logs for -Wunused-function/-Wunused-
   variable) is cheap and had a low false-positive rate once GLOB-based CMake matches and hidden-dir misses
   were accounted for.
2. Get a ruling on `tools/cross-alpha/build-libstack-alpha.sh` (keep as manual Alpha tool vs. delete).
3. Consider a deeper cross-TU "exported-but-never-called" sweep (needs `nm`/linker-map tooling per TU, or
   whole-program dead-symbol analysis) — not attempted this wave; likely low yield given how clean this
   codebase already is (the project retires scaffolds explicitly as they go, e.g. vms-d916, FC-P3.9), but
   it's the next rung if a deeper pass is wanted.
4. Do NOT re-attempt the `#if 0` grep inside `tests/corpus/**` as a removal target — those are vendored
   fixtures for the self-host build corpus, not OVMX's own code.

## RISKS
- None known. Both changes are small, isolated, and verified: full reconfigure + full rebuild (EXIT=0)
  and full local `ctest` (277/277 passed) after the changes, reproducing the pre-change baseline exactly.
- The real acceptance gate for the `tests/qemu/*` tier (kernel-executive / QEMU, incl. the file I edited)
  is the heavy multi-arch CI, not local ctest — I did not claim green there, only a clean local compile.
