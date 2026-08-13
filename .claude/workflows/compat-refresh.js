export const meta = {
  name: 'compat-refresh',
  description: 'Full re-census of the Compatibility Surface Register against origin/main, then render + credibility audit',
  whenToUse: 'Periodically or per-release, to re-ground docs/compat/ status against origin/main. See docs/compat/REFRESH.md.',
  phases: [
    { title: 'Census+Populate', detail: 'one agent per surface cluster measures origin/main and writes its facility YAML' },
    { title: 'Render', detail: 'regenerate the register and validate' },
    { title: 'Audit', detail: 'verify the verified/facade-risk credibility rules' },
  ],
}

// Full re-census of the Compatibility Surface Register.
// Runs the six-cluster fan-out that built the register, re-grounded against
// origin/main. Assumes docs/compat/ already exists (post-merge of the register).
// The durable procedure this automates is docs/compat/REFRESH.md.

const CLUSTERS = [
  { key: 'core-rtl', facilities: 'descriptors, status-codes, chf, lib, str, mth, ots, sys-time, sys-eventflags, sys-ast, sys-logicalnames, sys-process, sys-procinfo, sys-memory, sys-io, sys-mailbox, sys-lock, sys-fao-msg, sys-security (all domain programming-interfaces)' },
  { key: 'rms-files', facilities: 'rms-api (programming-interfaces); ods2, filespec, devices, logical-namespace, fdl (file-system)' },
  { key: 'dcl', facilities: 'dcl-verbs, dcl-scripting, dcl-qualifiers, lexicals, utilities, help, queues (command-language)' },
  { key: 'sec-sysmgmt', facilities: 'sysuaf, privileges, rights-db, protection-acl, audit, sysgen, boot, install, accounting (system-management)' },
  { key: 'clustering', facilities: 'kernel-executive (runtime-arch); scs, nisca, connection-manager, cluster-dlm, mscp-serve, cluster-logicals, shadowing (clustering)' },
  { key: 'toolchain-net', facilities: 'object-format, image-activation, symbol-vectors, link, librarian, macro, message-compiler, mms-mmk (toolchain); tcpip-services, decnet, lat, ssh (networking); smg (programming-interfaces). NOTE: only VMS surfaces belong here — OVMX self-hosting and architecture/platform bring-up are roadmap, not compatibility.' },
  { key: 'languages', facilities: 'compilers, language-rtl, calling-standard (languages) — VMS compilers (Fortran/COBOL/BASIC/Pascal/MACRO/Ada/PL/I/…), their FOR$/COB$/BAS$/PAS$ RTLs, and the OpenVMS Calling Standard. OVMX has only C (tcc); the rest are absent and mostly undecided for 1.0 (operator scope call). Do NOT scope a real VMS language out to protect the coverage number.' },
]

const CENSUS_SCHEMA = {
  type: 'object',
  required: ['files_written', 'check_line'],
  properties: {
    files_written: { type: 'array', items: { type: 'string' } },
    check_line: { type: 'string', description: 'final `render_compat.py --check` OK/ERROR line' },
    facade_risk_ids: { type: 'array', items: { type: 'string' } },
    verified_ids: { type: 'array', items: { type: 'string' } },
    notable_changes: { type: 'string', description: 'what moved vs the current YAML' },
  },
}

phase('Census+Populate')
const populated = await parallel(CLUSTERS.map((c) => () =>
  agent(
    `Re-census and re-populate the Compatibility Surface Register facility files for the "${c.key}" cluster, measured against origin/main.\n\n` +
    `Cluster facilities (token -> domain in the parens): ${c.facilities}.\n\n` +
    `GROUNDING RULE: the working checkout may be stale — measure ONLY against origin/main via \`git show origin/main:<path>\` and \`git grep <pat> origin/main\`. Never conclude status from a bare relative grep.\n\n` +
    `Read first: docs/design-compat-surface-register.md (data model), docs/compat/domains.yaml (vocab), docs/compat/facilities/str.yaml (gold-standard). Then, for each facility token, rewrite docs/compat/facilities/<token>.yaml to reflect current origin/main: status (absent|designed|stub|partial|implemented|verified), authenticity (real|advisory|facade-risk|n/a), evidence (a path on origin/main), scope_1_0, and last_reviewed=today. Enumerate corpus-critical facilities at routine granularity; every named gap is its own item.\n\n` +
    `CREDIBILITY RULES: status:verified REQUIRES verified_against naming a REAL oracle (lab-1/2/Alpha, mined captures, a CI negctl gate, a fixpoint) — NOT a plain local self-test; a partial-coverage facility can never be verified. authenticity:facade-risk ONLY where a surface reports success without doing the work / fakes shared state per-process.\n\n` +
    `Finally run \`python3 tools/compat/render_compat.py --check\` and FIX every ERROR (warnings about evidence-not-found on a stale checkout are expected). Return the required JSON.`,
    { label: `census:${c.key}`, phase: 'Census+Populate', schema: CENSUS_SCHEMA, agentType: 'general-purpose' }
  )
))

phase('Render')
const render = await agent(
  'Run `python3 tools/compat/render_compat.py` then `python3 tools/compat/render_compat.py --check`. Report both output lines verbatim. If --check prints any ERROR (not warning), list them. Do not edit any file.',
  { label: 'render+check', phase: 'Render', agentType: 'general-purpose' }
)

phase('Audit')
const audit = await agent(
  'Adversarially audit the freshly-populated Compatibility Surface Register for credibility, reading build/compat-surface.json.\n' +
  '1) Every item with status "verified" MUST have a verified_against that names a REAL oracle (lab/mined-capture/CI-negctl-gate/fixpoint), NOT merely a local tests/*.sh that asserts OVMX\'s own output; flag violators.\n' +
  '2) No facility whose coverage is "partial" should contain a "verified" item; flag any.\n' +
  '3) Spot-check that facade-risk items correspond to real success-without-work / fake-shared-state, not honest stubs (honest stubs are stub/advisory, not facade-risk).\n' +
  '4) SCOPE: every item must be a VMS compatibility surface. Its `vms:` field must name a VMS thing (a manual/service/command/format/behaviour), NOT an OVMX symbol, ioctl, init-wiring, deleted hack, dead-code note, or an engineering milestone (self-hosting, architecture/platform bring-up). Flag any item that is OVMX-internal or a roadmap concern rather than a VMS surface — it belongs on the roadmap, not this matrix.\n' +
  'Return a concise list of any items to correct, with the file, the item id, and the fix. If clean, say so.',
  { label: 'credibility-audit', phase: 'Audit', agentType: 'general-purpose' }
)

return {
  populated: populated.filter(Boolean),
  render,
  audit,
  note: 'Review the audit, apply any corrections, re-render, then land on a branch off origin/main (see docs/compat/REFRESH.md). At a release cut, tools/compat/snapshot.py stamps the coverage snapshot + notes block.',
}
