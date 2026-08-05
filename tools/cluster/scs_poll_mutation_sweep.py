#!/usr/bin/env python3
"""scs_poll_mutation_sweep.py -- vms-66f, review round 2.

THE MUTATION SWEEP FOR THE PROCESS POLLER'S PRODUCTION CODE.

tests/vmsscs/test_scs_poll.c states a kill count. This script IS that count.
It mutates src/vmsscs/scs_poll.c and src/vmsscs/scs_dir.c one edit at a time,
rebuilds, runs `ctest -L scs`, and requires the suite to go RED. A mutant that
stays green is a production branch no test constrains.

WHY IT IS NOT A ctest. Every mutant needs a full rebuild, so one sweep is
minutes, not seconds -- the same reason tools/scs_credit_measure.py is hand-run.
Its cheap sibling, tests/vmsscs/test_scs_dir_mutants.py, IS in ctest, because it
mutates only documents and re-runs a Python gate.

    ./tools/cluster/scs_poll_mutation_sweep.py [--build DIR] [ID ...]

Defaults to the `build` tree at the repo root; name mutant ids to run a subset.

TWO TRAPS THIS SCRIPT HANDLES, BOTH OF WHICH PRODUCED A WRONG ANSWER FIRST:

  1. A CONTROL RUN COMES FIRST. Without it every "KILLED" below could be a
     pre-existing red. The sweep refuses to score anything if the unmutated
     tree is not green.
  2. RESTORE MUST BUMP THE MTIME. shutil.copy2 preserves it, so a restored file
     looks OLDER than the object built from the mutant and make skips the
     rebuild -- leaving mutant binaries in the tree and reporting failures that
     the sources do not contain. os.utime after every restore.

Backups go to a caller-private scratch directory (mkdtemp), never a shared
/tmp path, and every restore is verified byte-for-byte with filecmp.
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TARGETS = ["src/vmsscs/scs_poll.c", "src/vmsscs/scs_dir.c"]

# (id, file-basename, old-text, new-text, what-it-attacks)
MUTANTS = [
    ("M01", "scs_poll.c", "return n->all_nodes || mac_eq(n->node, node);",
     "return 1;", "name_scopes: every name applies to every node"),
    ("M02", "scs_poll.c", "return n->all_nodes || mac_eq(n->node, node);",
     "return n->all_nodes;", "name_scopes: node-scoped names never apply"),
    ("M03", "scs_poll.c", "return n->all_nodes || mac_eq(n->node, node);",
     "return mac_eq(n->node, node);", "name_scopes: all-nodes names never apply"),
    ("M04", "scs_poll.c", "        unsigned i = (p->next_scan + k) % SCS_POLL_MAX_NODES;",
     "        unsigned i = k;", "round-robin fairness"),
    ("M05", "scs_poll.c", """        if (want == 0) {
            p->skipped_disabled++;
            continue;
        }""", """        if (want == 0) {
            continue;
        }""", "skipped_disabled accounting on the node scan"),
    ("M06", "scs_poll.c", """        if (nd->polled_once && (now_ms < nd->last_poll_ms ||
                                now_ms - nd->last_poll_ms < interval_ms)) {""",
     "        if (nd->polled_once && (now_ms - nd->last_poll_ms < interval_ms)) {",
     "clock-regression guard (unsigned underflow)"),
    ("M07", "scs_poll.c", """            if (p->state == SCS_POLL_DISCONNECTING) {
                p->disconnects_unclosed++;
            } else {
                p->cycles_abandoned++;
            }""", "            p->cycles_abandoned++;",
     "the two timeout failures counted apart"),
    ("M08", "scs_poll.c", """    if (answer == SCS_DIR_ANSWER_YES) {
        struct scs_poll_name *n = name_find(p, sysap);""",
     """    if (answer != SCS_DIR_ANSWER_NO) {
        struct scs_poll_name *n = name_find(p, sysap);""",
     "notify on Yes ONLY (p. 2-50)"),
    ("M09", "scs_poll.c", "        p->descriptors_forced++;", "        ;",
     "the forced-release counter"),
    ("M10", "scs_poll.c", """    if (p->emit == NULL) {
        p->port->unemitted++;
        return;
    }""", """    if (p->emit == NULL) {
        return;
    }""", "unemitted accounting with no emitter"),
    ("M11", "scs_poll.c", """    } else {
        p->port->refused++;
    }""", """    } else {
        p->port->unemitted++;
    }""", "REFUSED vs NOBUILDER kept apart"),
    ("M12", "scs_poll.c", """    if (n->disabled_count < SCS_POLL_MAX_NODES) {
        memcpy(n->disabled[n->disabled_count], node, 6);
        n->disabled_count++;
    }""", """    memcpy(n->disabled[n->disabled_count % SCS_POLL_MAX_NODES], node, 6);
    n->disabled_count++;""", "the disabled table refuses rather than evicts"),
    ("M13", "scs_poll.c", """    n->disabled_count = 0;
    n->on_found = on_found;""", "    n->on_found = on_found;",
     "a fresh request re-enables polling (p. 2-50)"),
    ("M14", "scs_poll.c", """    if (p->last_cycle_ms != 0 && now_ms >= p->last_cycle_ms &&
        now_ms - p->last_cycle_ms < SCS_POLL_ONE_AT_A_TIME_MS) {
        return;
    }""", "    (void)0;", "the ~1 s inter-cycle spacing (p. 2-50)"),
    ("M15", "scs_poll.c", """        if (t.illegal) {
            p->cycles_abandoned++;
            scs_poll_abandon(p);
            return 0;
        }""", "        (void)0;", "an illegal ACCEPT does not become an inquiry"),
    ("M16", "scs_poll.c", """        if (name_disabled_on(n, p->cur_node)) {
            p->skipped_disabled++;
            continue; /* p. 2-50: already connected on this node */
        }""", "        (void)0;", "a connected (SYSAP,node) pair is not asked again"),
    ("M17", "scs_poll.c", """    if (p->cdt == NULL) {
        p->state = SCS_POLL_IDLE;
        memset(p->cur_node, 0, 6);
        return;
    }
    (void)scs_disconnect(p->port, p->cdt, &a);""",
     """    if (p->cdt == NULL) {
        p->state = SCS_POLL_IDLE;
        memset(p->cur_node, 0, 6);
        return;
    }""", "the cycle really invokes DISCONNECT"),
    ("M18", "scs_poll.c", """        if (p != NULL) {
            p->answers_unsolicited++;
        }
        return 0;""", "        return 0;",
     "an answer with no cycle open is counted"),
    ("M19", "scs_poll.c", """    if (sent == 0) {
        /* Nothing to wait for. Closing now is the same rule as below, with an
         * empty inquiry set. */
        poll_close(p, now_ms);
    }""", "    (void)0;", "a cycle with nothing to wait for closes"),
    ("M20", "scs_poll.c", """    if (p->state != SCS_POLL_IDLE) {
        if (p->state == SCS_POLL_DISCONNECTING && p->cdt != NULL &&
            scs_svc_close_if_closed(p->port, p->cdt)) {""",
     """    if (p->state != SCS_POLL_IDLE) {
        if (p->cdt != NULL &&
            scs_svc_close_if_closed(p->port, p->cdt)) {""",
     "a closed CDT mid-inquiry is not a completed cycle"),
    ("M21", "scs_poll.c", """        if (p->port->cdl != NULL) {
            scs_cdl_release(p->port->cdl, p->cdt);
            p->port->cdts_released++;
        }""", "        (void)0;", "the descriptor really goes back to the CDL"),
    ("M22", "scs_poll.c", """    if (p == NULL || sysap == NULL || name_trim_len(sysap) == 0) {
        return 0;
    }""", """    if (p == NULL || sysap == NULL) {
        return 0;
    }""", "a blank SYSAP name is refused"),
    ("M23", "scs_poll.c", """            if (p->state != SCS_POLL_IDLE && mac_eq(p->cur_node, node)) {
                scs_poll_abandon(p);
            }""", "            (void)0;",
     "dropping the node under a cycle abandons it"),
    ("M24", "scs_poll.c", """        return name_scopes(n, node) && !name_disabled_on(n, node);""",
     "        return !name_disabled_on(n, node);",
     "scs_poll_polling honours the node scope"),
    ("M25", "scs_dir.c", "    /* [48:50] */ 0x03, 0x00,", "    /* [48:50] */ 0x00, 0x00,",
     "the CONNECT_REQ Send Credits byte is the captured 3"),
    ("M26", "scs_dir.c",
     "    /* [58:62] */ 0x00, 0x00, 0x00, 0x00,                   /* REQUEST marker (GROUNDED, sec 4h) */",
     "    /* [58:62] */ 0x01, 0x00, 0x00, 0x00,                   /* REQUEST marker (GROUNDED, sec 4h) */",
     "the request/response marker at [58:62]"),
]


def run(cmd):
    return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("ids", nargs="*")
    args = ap.parse_args()
    b = args.build

    ids = [m[0] for m in MUTANTS]
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        print("FAIL duplicate mutant ids: %r" % (dupes,))
        return 1

    bk = tempfile.mkdtemp(prefix="scs_poll_mutation_sweep.")
    try:
        for t in TARGETS:
            shutil.copy2(os.path.join(ROOT, t), os.path.join(bk, os.path.basename(t)))

        def restore():
            for t in TARGETS:
                src, dst = os.path.join(ROOT, t), os.path.join(bk, os.path.basename(t))
                shutil.copy2(dst, src)
                if not filecmp.cmp(src, dst, shallow=False):
                    raise SystemExit("restore mismatch: %s" % t)
                os.utime(src, None)   # copy2 preserves mtime; make would skip the rebuild

        restore()
        run("cmake --build %s -j%d" % (b, os.cpu_count() or 4))
        ctl = run("cd %s && ctest -L scs --output-on-failure" % b)
        if ctl.returncode != 0:
            print("CONTROL IS RED -- no kill below would mean anything:\n%s"
                  % ctl.stdout[-3000:])
            return 1
        print("control: unmutated tree is GREEN")

        killed, survived, unapplied = [], [], []
        for mid, fn, old, new, what in MUTANTS:
            if args.ids and mid not in args.ids:
                continue
            path = os.path.join(ROOT, "src", "vmsscs", fn)
            txt = open(path).read()
            if txt.count(old) != 1:
                unapplied.append((mid, txt.count(old)))
                continue
            open(path, "w").write(txt.replace(old, new))
            bres = run("cmake --build %s -j%d" % (b, os.cpu_count() or 4))
            if bres.returncode != 0:
                killed.append(mid)          # the compiler is a test too
                verdict = "KILLED (build)"
            else:
                t = run("cd %s && ctest -L scs" % b)
                if t.returncode != 0:
                    killed.append(mid)
                    verdict = "KILLED"
                else:
                    survived.append((mid, what))
                    verdict = "*** SURVIVED ***"
            print("%s: %-16s %s" % (mid, verdict, what))
            restore()

        restore()
        run("cmake --build %s -j%d" % (b, os.cpu_count() or 4))
        for mid, n in unapplied:
            print("FAIL mutant %s did not apply (anchor occurs %d times)" % (mid, n))
        for mid, what in survived:
            print("FAIL mutant %s SURVIVED -- uncovered: %s" % (mid, what))
        print("%d mutants, %d killed, %d survived, %d failed to apply"
              % (len(killed) + len(survived) + len(unapplied),
                 len(killed), len(survived), len(unapplied)))
        return 1 if (survived or unapplied) else 0
    finally:
        shutil.rmtree(bk, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
