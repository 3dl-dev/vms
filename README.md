# Untracked work rescued during the baron-surface -> workshop migration (2026-08-01)

`git stash create` (used to snapshot the 45 dirty worktrees as `wip/migration-20260801/*`
tags) captures **tracked** changes only. These files were untracked and would
otherwise have been left behind on baron-surface. Original worktree path is the
top-level directory name here.

- `wf_f233faf7-267-2/tests/integration/test_host_identity_census{,_negctl}.sh`
  — 664 lines of new integration tests. Present in **no** commit anywhere in the
  repo history. Highest-value item in this branch.
- `wf_47880dbc-c4b-1/adv8/*.sh` — undefined-symbol measurement / check scripts.
- `vms-rules/adv8/lab/` — adversarial-round experiment records (.out/.txt +
  their generator scripts).

Deliberately excluded as derived, not original: `vms-rules/adv8/build/` (87 MB of
CMake output), `vms-rules/adv8/tree/` (a checked-out copy of the repo), and
`vms-rules/adv8/gatecheck{,2,3,4}/` (copies of src/tests/tools).
