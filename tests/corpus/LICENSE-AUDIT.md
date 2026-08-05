# License Audit

This file records the license status of each corpus source and whether it is
usable for the OVMX conformance test harness.

Audited: 2026-02-19

---

## Summary

| Source | License | Can use for testing | Can commit to repo | Restrictions |
|--------|---------|--------------------|--------------------|--------------|
| tier1-examples (Eight-Cubed) | Eight-Cubed custom | YES | With caution | No modified distribution; verbatim only |
| tier3-mmk (MMK) | BSD 3-Clause | YES | YES | Attribution required |
| tier3-netlib (NETLIB) | All Rights Reserved | YES (read-only corpus) | With caution | No redistribution rights confirmed |
| tier4-mx (MX) | BSD 3-Clause (BLISS); mixed C | YES | YES | Attribution required |
| tier6-laxdriver (vms-laxdriver) | MIT | YES | YES | Attribution required |
| tier6-ipc-benchmark (vms-ipc_benchmark) | MIT | YES | YES | Attribution required |
| tier6-memtester (vms-memtester) | GPL-2.0 | YES | YES | Source availability (GPL) |
| tier6-cmatrix (cmatrix) | GPL-3.0 | YES | YES | Source availability (GPL) |

---

## tier1-examples/ — Eight-Cubed VMS C Examples

**License type:** Eight-Cubed custom (non-OSI)
**License URL:** http://www.eight-cubed.com/disclaimer.html
**SPDX:** None (proprietary with limited distribution grant)
**Copyright:** Copyright 2003-2023 James F. Duff. ALL RIGHTS RESERVED.

### License text summary

The author grants:
- Free copying and distribution of **verbatim** unmodified copies
- Modification **for your own use** (private/internal)

The author prohibits:
- Distribution of **modified** copies

### OVMX usage assessment

**Status: USABLE for test corpus — with constraints**

These files are used as **read-only reference inputs** to the conformance
harness. OVMX does not modify them, does not distribute modified versions,
and does not claim authorship. The corpus is used to test whether the OVMX
toolchain can accept real VMS C source code — analogous to using published
code as test cases.

**Constraints:**
1. Do NOT modify any `.c` file in `tier1-examples/`. Use them verbatim only.
2. If the harness needs a wrapper or driver, put it in a separate file —
   do not inline-modify the Eight-Cubed source.
3. Copyright notices in each file (`/* Copyright 2003-2023 James F. Duff */`)
   must be preserved.
4. The corpus directory should not be published as a standalone "collection"
   that could be construed as redistribution of a modified collection.
   It lives inside the OVMX test infrastructure for internal testing purposes.

**Risk level: LOW** — read-only corpus usage; no modification; no standalone
distribution. This matches the license grant.

---

## tier3-mmk/ — MMK (MadGoat Make)

**License type:** BSD 3-Clause
**License file:** `tier3-mmk/license.txt`
**SPDX:** BSD-3-Clause
**Copyright:** Copyright (c) 2008, Matthew Madison; Copyright (c) 2012/2014,
Endless Software Solutions.

### License text summary

Standard BSD 3-Clause:
- May use, copy, modify, and distribute with attribution
- Redistributions must retain copyright notice and disclaimer
- May not use author names for endorsement without permission

### OVMX usage assessment

**Status: FULLY USABLE**

BSD 3-Clause is compatible with OVMX's development and testing use. Files
may be committed to the OVMX repo with their license notices intact.

**Requirements:**
- Preserve `license.txt` in `tier3-mmk/`
- Preserve copyright headers in individual source files

---

## tier3-netlib/ — NETLIB

**License type:** All Rights Reserved (no open-source license identified)
**License file:** None found. Source headers read:
  `COPYRIGHT (C) 1993,1997,2004 MADGOAT SOFTWARE. ALL RIGHTS RESERVED.`
**GitHub classification:** "Other" (NOASSERTION)
**SPDX:** None

### License text summary

No explicit license grant found in the repository. The copyright header
claims all rights reserved. However:

- The repository is publicly available on GitHub under the `endlesssoftware`
  organization (the same organization that explicitly BSD-3-licensed MMK and
  MX)
- Public GitHub availability implies at minimum viewing/forking rights under
  GitHub's Terms of Service

### OVMX usage assessment

**Status: USABLE as read-only corpus — with caution**

Use NETLIB source files as **read-only test inputs** only. Do not modify,
do not claim authorship, do not publish derivatives. If a cleaner license
situation is required, contact the Endless Software Solutions maintainers to
request explicit permission or a license clarification.

**Risk level: MEDIUM** — no explicit redistribution grant. Treat like the
Eight-Cubed files: verbatim, unmodified, internal test use only.

**Action item:** If OVMX is ever publicly released with this corpus, obtain
explicit written permission from the NETLIB maintainers or remove this source
from the corpus.

---

## tier4-mx/ — MX Email Server

**License type:** BSD 3-Clause (per source file headers)
**License file:** No standalone LICENSE file; per-file headers in all BLISS
and C modules state BSD 3-Clause terms (Matthew Madison, 2008+)
**SPDX:** BSD-3-Clause
**Copyright:** Copyright (c) 2008, Matthew Madison; Endless Software Solutions.

Note: The C files in `common/` (`regcomp.c`, `regexec.c`, `regfree.c`,
`regerror.c`) are from NetBSD, copyright Henry Spencer and UC Regents —
BSD 4-Clause with advertising clause (historical BSD). These are compatible
with read-only corpus use.

### OVMX usage assessment

**Status: FULLY USABLE**

BSD 3-Clause (and BSD 4-Clause for the NetBSD-derived files) permits use,
copy, and redistribution with attribution. Compatible with OVMX testing.

**Requirements:**
- Preserve per-file copyright headers
- Preserve the advertising clause requirement for the NetBSD-derived files
  if redistributing (include acknowledgement of Henry Spencer / UC Regents)

---

## tier6-laxdriver/ — vms-laxdriver

**License type:** MIT
**License file:** `tier6-laxdriver/LICENSE`
**SPDX:** MIT
**Copyright:** Jake Hamby, 2022

### OVMX usage assessment

**Status: FULLY USABLE.** MIT permits use, copy, modify, and distribute
with attribution. Preserve `LICENSE` and file headers.

---

## tier6-ipc-benchmark/ — vms-ipc_benchmark

**License type:** MIT
**License file:** `tier6-ipc-benchmark/LICENSE`
**SPDX:** MIT
**Copyright:** Jake Hamby, 2023

### OVMX usage assessment

**Status: FULLY USABLE.** Same terms as tier6-laxdriver.

---

## tier6-memtester/ — vms-memtester

**License type:** GPL-2.0
**License file:** `tier6-memtester/COPYING`
**SPDX:** GPL-2.0
**Copyright:** Charles Cazabon (original memtester); Jake Hamby (VMS port), 2022

### OVMX usage assessment

**Status: FULLY USABLE.** GPL-2.0 permits use, copy, modify, and
redistribution; redistributed source must remain under GPL-2.0 with
`COPYING` intact. Compatible with read-only corpus use and with commit to
the repo (OVMX is not linking this code into its own binaries, only using
it as a compile/run test target).

---

## tier6-cmatrix/ — cmatrix

**License type:** GPL-3.0
**License file:** `tier6-cmatrix/COPYING`
**SPDX:** GPL-3.0
**Copyright:** Chris Allegretta (original cmatrix); Jake Hamby (VMS port), 2021

### OVMX usage assessment

**Status: FULLY USABLE.** Same reasoning as tier6-memtester, under GPL-3.0.

---

## Round 2 acquisition (vms-e86) — items NOT committed

Everything else discovered in the `vms-e86` breadth pass (WASD, OpenSSL,
curl, plibsys, GNV forks, John Francis PCSI kits, Digiater freeware CD,
vms-ports SourceForge, further jhamby ports, both vmsbackup
implementations, VSI's own freeware/PCSI kits, hobbyist OS ISOs) was
either too large to commit (repo size discipline) or has unclear/no
declared license. None of it was added to git. Full per-item license
status is in `tests/corpus/inventory.json` and the flagged-list table in
`tests/corpus/INVENTORY.md`. Fetch scripts for the redistributable-but-large
items live in `tests/corpus/fetch/`; nothing under `fetch/` should ever be
run automatically as part of a build or test — it is a manual/documented
retrieval path only, landing outside git (see each script's `DEST`
handling, which defaults to a scratch staging directory, never the repo).

---

## Overall Corpus Assessment

The corpus is suitable for the OVMX conformance test harness with the
following standing policy:

1. **No modification of corpus files.** The harness wraps them; it does not
   alter them. This keeps Eight-Cubed and NETLIB usage within their license
   grants.

2. **Attribution preserved.** Copyright headers in all files are left intact.

3. **NETLIB caveat.** If OVMX corpus is ever distributed publicly (e.g. as
   part of a release tarball), remove `tier3-netlib/` or obtain explicit
   permission first.

4. **Eight-Cubed caveat.** Same: do not distribute the corpus directory as a
   standalone collection. It is test infrastructure, not a redistributed
   software package.
