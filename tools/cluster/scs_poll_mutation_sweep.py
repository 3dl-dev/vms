#!/usr/bin/env python3
"""scs_poll_mutation_sweep.py -- vms-66f, review round 2.

THE MUTATION SWEEP FOR THE PROCESS POLLER'S PRODUCTION CODE.

tests/vmsscs/test_scs_poll.c states a kill count. This script IS that count.
It mutates src/vmsscs/scs_poll.c, src/vmsscs/scs_dir.c and src/vmsscs/scsd.c
(the poller's port-driver half) one edit at a time,
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
TARGETS = ["src/vmsscs/scs_poll.c", "src/vmsscs/scs_dir.c", "src/vmsscs/scsd.c"]

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
    # RE-ANCHORED (round 4). The anchor used to carry the whole descriptorless
    # early return above this line as context; round 4 documented that block and
    # the anchor stopped matching, which the sweep reported as "did not apply".
    # The single line is UNIQUE in scs_poll.c (checked), so it is the whole
    # anchor now -- less prose to collide with, same deletion, same claim.
    ("M17", "scs_poll.c", "    (void)scs_disconnect(p->port, p->cdt, &a);",
     "    (void)0;", "the cycle really invokes DISCONNECT"),
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
    # RE-ANCHORED (round 4), same cause as M17: round 4 put a comment between
    # the enclosing `state != IDLE` guard and this condition. The two-line
    # condition is UNIQUE in scs_poll.c (checked) and does not need the guard
    # line as context.
    ("M20", "scs_poll.c", """        if (p->state == SCS_POLL_DISCONNECTING && p->cdt != NULL &&
            scs_svc_close_if_closed(p->port, p->cdt)) {""",
     """        if (p->cdt != NULL &&
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

    # --- review round 3: the two arms gcov found UNCOVERED ------------------
    # Both were measured over the whole `ctest -L scs` suite: poll_emit_action()
    # was entered 34 times with the SENT arm taken 0 times, and the compaction
    # loop guard was evaluated 20 times with its body executed 0 times. They are
    # in the sweep so the two new cases in test_scs_poll.c cannot rot back into
    # decoration -- run them with `./tools/cluster/scs_poll_mutation_sweep.py
    # M27 M28`.
    ("M27", "scs_poll.c", """    if (r == SCS_SVC_EMIT_SENT) {
        p->port->emitted++;""", """    if (r == SCS_SVC_EMIT_SENT) {
        ;""",
     "a SENT emit is credited to port->emitted"),
    ("M28", "scs_poll.c",
     "        memcpy(p->pending[i], p->pending[i + 1], SCS_DIR_NAME_LEN + 1);",
     "        ;",
     "the pending list COMPACTS when a middle inquiry is answered"),

    # --- review round 4: the p. 2-50 TEARDOWN, and the three cycle endings it
    # was suppressing. scsd_poll_emit() answered NOBUILDER for
    # SEND_DISCONNECT_REQ on the false ground that OVMX has no such builder, so
    # no teardown left the daemon and gcov showed the tick() clean-release arm
    # and BOTH poll_close() early returns executed 0 times over the whole
    # `ctest -L scs` suite. M29 attacks the frame, M30..M33 the four endings.
    # scsd.c joins the mutation targets for M29/M30 -- it is where the emitter
    # lives, and a sweep that cannot reach it cannot defend the fix.
    ("M29", "scsd.c", """    if (act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        struct peer_state *dps = scsd_peer_by_sys(rx, args->target_node);""",
     """    if (0 && act == SCS_CONN_ACT_SEND_DISCONNECT_REQ) {
        struct peer_state *dps = scsd_peer_by_sys(rx, args->target_node);""",
     "the poller's cycle-closing DISCONNECT_REQ is actually built and sent"),
    ("M30", "scsd.c", """                        if (rel && scsd_poller_ready &&
                            scs_poll_cdt_released(&scsd_poller, tgt)) {""",
     """                        if (0 && rel && scsd_poller_ready &&
                            scs_poll_cdt_released(&scsd_poller, tgt)) {""",
     "the receive path PUSHES the release to the owning poller"),
    ("M31", "scs_poll.c", """            p->cdt = NULL;
            p->state = SCS_POLL_IDLE;
            p->disconnects_closed++;""", """            p->cdt = NULL;
            p->state = SCS_POLL_IDLE;
            ;""",
     "tick()'s clean release COUNTS the completed teardown"),
    ("M32", "scs_poll.c", """    if (p->state == SCS_POLL_DISCONNECTING) {
        p->disconnects_closed++;
    } else if (p->state != SCS_POLL_IDLE) {
        p->cycles_abandoned++;
    }""", """    if (p->state == SCS_POLL_DISCONNECTING) {
        p->disconnects_closed++;
    }""",
     "a pushed release MID-CYCLE is an abandoned cycle, not a silent one"),
    ("M33", "scs_poll.c", """    if (p == NULL || cdt == NULL || p->cdt != cdt) {
        return 0;
    }""", """    if (p == NULL || cdt == NULL) {
        return 0;
    }""",
     "scs_poll_cdt_released only claims the poller's OWN descriptor"),
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
