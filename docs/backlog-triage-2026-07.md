# Backlog Triage — pivot to cluster interop (vms-pivot.3, 2026-07-25)

## The objective (do not narrow this)

The win is **VSI's entire enclosure snipped** — every capability they gate,
reproduced and given away free, until there is nothing left to charge for. Not
just clustering. *Everything.* Languages (Fortran, BASIC, BLISS, COBOL), DECnet,
full DCL, RMS, the utilities, the compatibility long tail — all of it is the
barbed wire, and freeing all of it is the objective.

**Clustering is the sharpest wedge, not the objective.** It's the cut that makes
migration zero-downtime and forces the first "their fence is broken" moment. But
it is one blade of many. Nothing on this backlog is "unnecessary to the
objective" — the objective is total commoditization of VMS.

Consequence for this triage: **nothing is deferred, nothing is cancelled.** Every
open item is in scope. The only output is a *focus order* — what we drive first —
not a cull.

## Rail reconciliation
`vms-913` (image activation) and `vms-ci` (cluster interop) are co-equal parallel
pillars; neither defers. They converge at the evacuation demo (`vms-ci.6`).

## Focus order (sequencing, not exclusion — everything stays active)

### Tier 1 — The wedge (drive now)
The sharpest cut + the ability to run their software on the freed node.
- Cluster interop: `vms-ci` tree (`.0 .1 .2 .3 .4 .5 .6 .7 .8`) + prereqs `.7` (rewire $ENQ), `.8` (node identity)
- Image activation: `vms-913` tree (`.2` P0 …)
- Provable source compatibility: `vms-801` tree (running real apps)
- Authenticity / no-Unix-leaks: `vms-898`, `vms-898.11`, `vms-843`

### Tier 2 — The compatibility surface (the "everything… gone")
All in scope, actively pursued, sequenced behind the wedge — NOT deferred:
- **Languages**: `vms-96y` Fortran, `vms-xsl` BASIC, `vms-c6l` BLISS — VMS shops run these; freeing them is core to the objective
- **DECnet Phase IV**: `vms-eat` — core VMS networking; part of the fence
- **Full DCL**: `vms-802 803 804 805` pipes, `vms-h6e` — authenticity + completeness
- **Install / boot / run-as-node**: `vms-827 835 841`, `vms-p78` tree, `vms-u78`, `vms-obb`, `vms-2l8`, `vms-831`
- **Auth/identity**: `vms-846` tree (SYSUAF→RMS)
- **Utilities / drivers / integration**: `vms-905`, `vms-5yk` Buildroot, `vms-e9x` FUSE ODS-2, `vms-zwq` Rightslist

### Foundation / hygiene (ongoing, underpins everything)
- Test coverage: `vms-814 815 816 817 818 819 820 821 822`
- Security/bug fixes (audit `vms-912`): `vms-867 888 889 887 883 884 890 891 893 894 895 896 897`
- Docs: `vms-6as`, `vms-cww`

### Content pipeline (unchanged)
`vms-823 824 847 848` (blog candidates)

## Deferred / cancelled: NONE
The pivot reorders focus; it does not cull the mission. Every gate VSI built is a
target.
