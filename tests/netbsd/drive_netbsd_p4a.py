#!/usr/bin/env python3
#
# drive_netbsd_p4a.py - Phase 4-A of epic vms-8e8 (rd vms-f8a, parent vms-dd8).
#
# P2c (drive_netbsd_p2c.py) proved ONE executive facility -- event flags -- is
# real, shared, cross-process kernel state on the OVMX/NetBSD `vms' pseudo-
# device. The four remaining facility BACKENDS (ASTs, access modes, mailboxes,
# process table, locks -- vms-9bb/vms-ca7/vms-d7a/vms-ff7) have since landed in
# src/kernel-netbsd/ and are already cross-compiled per-PR
# (tests/netbsd/crosscompile.sh), but until this driver NONE of them had ever
# been BUILT, LOADED and EXERCISED CROSS-PROCESS against a real, booted
# /dev/vms under QEMU -- the runtime half of the proof P2c already gave event
# flags. This driver closes that gap for all five remaining facilities:
#
#   PROCTAB: process A $SETPRNs itself and stays alive; a DIFFERENT process B
#     resolves it by name with $GETJPI, and a THIRD process enumerates the
#     table with $PROCESS_SCAN and finds the same row -- the name lives in the
#     executive's shared process table (src/kernel-core/vms_proctab.c), not in
#     either reader.
#
#   MBX: process A $CREMBXes a PERMANENT mailbox (so it outlives A); a
#     DIFFERENT process B writes a message into it BY NAME; a THIRD process C
#     reads the SAME bytes back -- the message queue lives in the executive
#     (src/kernel-core/vms_mbx.c), not in any of the three processes.
#
#   LOCK (DLM): process A $ENQs an EXCLUSIVE lock on a named resource and
#     holds it; a DIFFERENT process B's synchronous $ENQW for the same
#     resource/mode BLOCKS IN THE KERNEL (not "the executive said busy" --
#     it does not even return) until A $DEQs, at which point B's blocked call
#     unblocks and is granted -- the resource database + wait queue live in
#     the executive (src/kernel-core/vms_lock.c).
#
#   ACCESS MODES: process A $SETPRVs a real, permanent privilege change on
#     itself and stays alive; a DIFFERENT process B's $GETJPI on A's name
#     reads back the SAME mutated privilege mask -- the mask lives in the
#     executive's process table entry (src/kernel-core/vms_access.c writes
#     it; src/kernel-core/vms_proctab.c serves it), not in either process.
#
#   AST: $DCLAST is SELF-DIRECTED by VMS design (an AST is declared to the
#     calling process), so there is no direct "A writes, B reads" case for
#     the AST queue itself. What IS cross-process, and is what this driver
#     proves, is the mailbox facility's WRITE-ATTENTION integration
#     (src/kernel-core/vms_mbx.c calling vms_ast_notify_arrival,
#     src/kernel-core/vms_ast.c): process A arms a write-attention AST on a
#     mailbox and $HIBERs (blocks in-kernel); a DIFFERENT process B's write
#     to that SAME mailbox is what lands the AST in A's executive-resident
#     queue and WAKES A's $HIBER -- proving both the AST queue and the
#     HIBER/wake path are real, shared kernel state a per-process fake could
#     never be woken by.
#
# Together with P2c's event-flag proof, this driver completes "every
# executive facility has a working NetBSD backend, exercised cross-process
# against the real NetBSD `vms' pseudo-device" (vms-f8a).
#
# INV-6 (CLAUDE.md Rule 9): before the module is loaded, EVERY new probe tool
# (vmsproctab, vmsmbx, vmslock, vmsaccess) is run once and must fail HONESTLY
# (SS$_NOSUCHDEV), never fake success -- the same discipline P2c established
# for vmseflag.
#
# NEGATIVE CONTROL (teeth, P4A_SKIP_PROCTAB_BG=1): process A is never started,
# so process B's $GETJPI-by-name lookup must come back SS$_NONEXPR / not
# found -- proving the positive "B sees A" assertion is not vacuous.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting
# a real NetBSD in QEMU to build and load a real kernel module and TEST it is
# exactly what tests/qemu/ does for the Linux vms.ko executive.

import os
import sys
import signal
import subprocess
import traceback

import anita

import netbsd_console


def log(msg):
    print("[drive_netbsd_p4a] %s" % msg, flush=True)


def env(name, default=None):
    v = os.environ.get(name)
    return v if (v is not None and v != "") else default


def sha512_of(path):
    import hashlib
    h = hashlib.sha512()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_boot_iso(workdir, arch, iso_name, expected_sha512):
    iso = os.path.join(workdir, "download", arch, "installation", "cdrom", iso_name)
    if not os.path.exists(iso):
        log("FAIL: expected boot ISO not found where anita mirrors it: %s" % iso)
        return False
    got = sha512_of(iso)
    if got.lower() != expected_sha512.lower():
        log("FAIL: %s SHA512 mismatch" % iso_name)
        log("  expected: %s" % expected_sha512)
        log("  got:      %s" % got)
        return False
    log("OK: %s SHA512 verified (%s...)" % (iso_name, got[:16]))
    return True


def kvm_available():
    return os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.R_OK | os.W_OK)


def accel_args():
    if kvm_available():
        log("acceleration: KVM (/dev/kvm present and writable)")
        return ["-accel", "kvm", "-cpu", "host", "-smp", "4"]
    log("acceleration: TCG (no usable /dev/kvm) -- build/boot will be slower")
    return ["-smp", "2"]


# ---- deterministic console (rd vms-2d9) --------------------------------------
_con = None


def _console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def wait_for_login(child, boot_deadline):
    _console(child).wait_for_login(boot_deadline)


def build_source_iso(guest_src_dir, out_iso):
    if not os.path.isdir(guest_src_dir):
        raise RuntimeError("guest source dir missing: %s" % guest_src_dir)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXSRC",
           "-o", out_iso, guest_src_dir]
    log("building source ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("source ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


def run(child, cmd, timeout, echo=True, retriable=True, bg_safe=False):
    """Run one /bin/sh command in the guest; return (exit_status, output)."""
    return _console(child).run(cmd, timeout, echo, retriable, bg_safe)


def login(child, cmd_timeout):
    _console(child).login_root_sh(cmd_timeout)


def phase_token(out, key):
    """Return VALUE from the LAST `KEY=VALUE' line a collapsed phase script
    emitted on its own line (the single result token the harness keys on, so a
    verbose phase crosses the lossy serial as one small token instead of ~5-7
    interactive round-trips). Last match wins so an echoed command line cannot
    shadow the real result."""
    val = None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith(key + "="):
            val = s[len(key) + 1:].strip()
    return val


def parse_token(out, key):
    """Pull VALUE out of a 'KEY=VALUE' token in console output (first match,
    whitespace-delimited)."""
    for line in out.splitlines():
        idx = line.find(key + "=")
        if idx < 0:
            continue
        rest = line[idx + len(key) + 1:]
        return rest.split()[0].strip()
    return None


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "amd64")
    url = env("NETBSD_URL",
              "https://cdn.netbsd.org/pub/NetBSD/NetBSD-%s/%s/" % (version, arch))
    iso_name = env("NETBSD_BOOT_ISO", "boot-com.iso")
    iso_sha512 = env("NETBSD_BOOT_ISO_SHA512", "")

    # SHARED installed-disk cache (rd vms-2d9): identical workdir + sets across
    # every NetBSD/amd64 driver, so one cached wd0.img serves all of them. The
    # OVMX module + probes are built in-guest after boot, never baked into the
    # disk.
    workdir = env("NETBSD_WORKDIR",
                  "/cache/anita-netbsd-shared-%s-%s" % (version, arch))

    guest_src_dir = env("OVMX_GUEST_SRC", "/netbsd/guest-src")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-src-p4a.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "2400"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "1200"))
    build_timeout = int(env("NETBSD_BUILD_TIMEOUT", "1800"))
    # Bounded poll budgets (seconds) for the cross-process rendezvous points
    # below -- generous for a legitimately slow nightly TCG run, but never
    # unbounded (a real failure must redden the job, not hang it; the harness's
    # own hard `timeout` in run_p4a.sh is the final backstop).
    poll_budget = int(env("NETBSD_POLL_BUDGET", "30"))

    skip_proctab_bg = bool(env("P4A_SKIP_PROCTAB_BG"))

    prime_only = bool(env("PRIME_ONLY"))

    log("NetBSD %s/%s  (OVMX/NetBSD P4-A: proctab+mbx+lock+access+ast cross-process)"
        % (version, arch))
    log("release URL:   %s" % url)
    log("work/cache dir: %s" % workdir)
    if skip_proctab_bg:
        log("NEGATIVE CONTROL: P4A_SKIP_PROCTAB_BG set -- process A never "
            "registers a name; process B's GETJPI-by-name lookup must go RED")

    p4a_sets = ["kern-GENERIC", "modules", "base", "etc", "comp", "syssrc"]

    a = anita.Anita(
        dist=anita.URL(url, sets=p4a_sets),
        workdir=workdir,
        memory_size="1024M",
        disk_size="8G",
        persist=True,
        vmm_args=accel_args(),
    )

    if not prime_only:
        build_source_iso(guest_src_dir, src_iso)

    child = None
    try:
        import pexpect

        log("installing NetBSD with comp+syssrc (cache-aware)...")
        a.install()
        log("install step complete (image present)")

        if iso_sha512:
            if not verify_boot_iso(workdir, arch, iso_name, iso_sha512):
                return 3
        else:
            log("WARNING: no NETBSD_BOOT_ISO_SHA512 provided; skipping ISO verify")

        if prime_only:
            log("PRIME_ONLY: shared NetBSD disk is installed and present -- "
                "cache primed, exiting 0 (no boot/build/proof)")
            return 0

        a.persist = False
        cd_args = ["-drive",
                   "file=%s,media=cdrom,readonly=on" % os.path.abspath(src_iso)]
        log("booting installed image (snapshot overlay; deadline %ds) with the "
            "source ISO attached..." % boot_deadline)
        child = a.start_boot(vmm_args=cd_args)
        wait_for_login(child, boot_deadline)
        a.child = child

        login(child, cmd_timeout)
        # Tell anita we are already logged in: this driver logs in itself (via the
        # custom-prompt console driver), so anita's own a.halt() must NOT re-run
        # its login(), which expects the default `login:'/`# ' prompts and would
        # block forever against our unique prompt -- leaving the final a.halt()
        # hung until the per-command deadline and starving the "ALL CHECKS PASSED"
        # banner even though every proof already passed (rd vms-f8a). With this
        # set, a.halt() short-circuits login() and just sends `halt' + waits
        # (bounded) for the shutdown confirmation.
        a.is_logged_in = True
        log("logged in")

        # ---- stage the sources from the CD --------------------------------
        rc, _ = run(child,
                    "mkdir -p /root/ovmx && "
                    "{ mount_cd9660 /dev/cd0a /mnt || "
                    "  mount_cd9660 /dev/cd1a /mnt || "
                    "  mount_cd9660 /dev/cd0d /mnt ; } && "
                    "cp -R /mnt/kmod /mnt/probe /mnt/kernel-core /root/ovmx/ && "
                    "umount /mnt && chmod -R u+w /root/ovmx && "
                    "test -f /root/ovmx/probe/vmsproctab.c",   # quiet: no ls -R
                    cmd_timeout)
        if rc != 0:
            log("FAIL: could not stage OVMX sources from the CD in the guest")
            return 10

        # ---- build the kernel module (now carries ALL SIX facilities) -----
        rc, out = run(child,
                      "cd /root/ovmx/kmod && make clean >/dev/null 2>&1; "
                      "make >/tmp/kmodbuild.log 2>&1 && echo KMOD_BUILD_OK "
                      "|| cat /tmp/kmodbuild.log",
                      build_timeout)
        if "KMOD_BUILD_OK" not in out:
            log("FAIL: in-guest kernel-module build failed (diagnostic above)")
            return 11
        rc, _ = run(child, "test -f /root/ovmx/kmod/vms.kmod", cmd_timeout)
        if rc != 0:
            log("FAIL: build reported success but vms.kmod is not present")
            return 12
        log("OK: vms.kmod built in-guest -- ALL SIX shared executive facility "
            "sources (eflag, ast, access, mbx, proctab, lock), compiled with "
            "the NetBSD backend")

        # ---- build the four new cross-process probe tools ------------------
        rc, out = run(child,
                      "cd /root/ovmx/probe && "
                      "cc -O -Wall -Wextra -I. -o vmsproctab "
                      "vmsproctab.c kif_transport_netbsd.c "
                      "> /tmp/toolbuild.log 2>&1 && "
                      "cc -O -Wall -Wextra -I. -o vmsmbx "
                      "vmsmbx.c kif_transport_netbsd.c "
                      ">> /tmp/toolbuild.log 2>&1 && "
                      "cc -O -Wall -Wextra -I. -o vmslock "
                      "vmslock.c kif_transport_netbsd.c "
                      ">> /tmp/toolbuild.log 2>&1 && "
                      "cc -O -Wall -Wextra -I. -o vmsaccess "
                      "vmsaccess.c kif_transport_netbsd.c "
                      ">> /tmp/toolbuild.log 2>&1 && "
                      "echo TOOLS_BUILD_OK || cat /tmp/toolbuild.log",
                      build_timeout)
        if "TOOLS_BUILD_OK" not in out:
            log("FAIL: in-guest probe-tool build failed (diagnostic above)")
            return 13
        log("OK: vmsproctab/vmsmbx/vmslock/vmsaccess built in-guest (through "
            "kif_transport_netbsd.c)")

        PT = "/root/ovmx/probe/vmsproctab"
        MB = "/root/ovmx/probe/vmsmbx"
        LK = "/root/ovmx/probe/vmslock"
        AC = "/root/ovmx/probe/vmsaccess"
        PB = poll_budget
        TX = "/tmp/p4a_tx"     # in-guest transcript, cat'd ONCE at the end

        # LOSS-TOLERANT TRANSPORT (rd vms-f8a). The emulated TCG serial drops
        # output intermittently; with ~40 interactive per-command markers per run,
        # some marker is lost almost every cold run, and per-marker robustness
        # never reaches zero loss. So each PHASE below runs as ONE in-guest
        # command that does all its steps (including backgrounding process A and
        # killing it), asserts every INV-6 property IN-GUEST over reliable local
        # pipes, appends a full transcript to TX, and emits exactly ONE result
        # token. The harness awaits that single token (retriable to the overall
        # deadline, since each phase is idempotent -- it cleans up its own helper
        # before the token), then prints the human/CI assertion line. INV-6 is
        # unchanged: every probe still opens /dev/vms and each property is still
        # asserted -- only the transport is collapsed. The full transcript is read
        # once at the very end for evidence.
        run(child, "rm -f %s" % TX, cmd_timeout)

        # ---- INV-6 NEGATIVE CONTROL (module absent): ONE token for all four ----
        neg = ("rm -f /dev/vms; F=''; "
               "for S in 'vmsproctab getjpi_name nobody' 'vmsmbx create_hold 1' "
               "'vmslock hold_release NORES 0 0' 'vmsaccess selftest'; do "
               "O=$(/root/ovmx/probe/$S 2>&1); R=$?; "
               "{ echo \"### negctl $S rc=$R\"; echo \"$O\"; } >>%s; "
               "[ $R -eq 0 ] && F=\"$F faked[$S]\"; "
               "echo \"$O\" | grep -q 'NOT faking success' || F=\"$F nohonest[$S]\"; "
               "done; "
               "[ -z \"$F\" ] && echo NEGCTL=ALL_HONEST || echo \"NEGCTL=FAIL:$F\""
               % TX)
        rc, out = run(child, neg, cmd_timeout)
        if phase_token(out, "NEGCTL") != "ALL_HONEST":
            log("FAIL (INV-6): module-absent honest-failure negctl did not pass "
                "for every probe: %s" % phase_token(out, "NEGCTL"))
            return 20
        log("OK (INV-6): all four new probe tools FAILED HONESTLY "
            "(SS$_NOSUCHDEV) with no module loaded -- none faked success")

        # ---- load the module + create the node -----------------------------
        # Made IDEMPOTENT (so it is retriable to the deadline like every other
        # phase): a leading `modunload' drops any module a lost-marker retry left
        # loaded, and /dev/vms is recreated fresh. Emits one MODLOAD token.
        rc, out = run(child,
                      "modunload vms 2>/dev/null; "
                      "modload /root/ovmx/kmod/vms.kmod && "
                      "MAJ=`dmesg | sed -n "
                      "'s/.*vms: registered, char major \\([0-9][0-9]*\\).*/\\1/p'"
                      " | tail -1` && "
                      "test -n \"$MAJ\" && rm -f /dev/vms && "
                      "mknod /dev/vms c $MAJ 0 && chmod 666 /dev/vms && "
                      "test -c /dev/vms && echo MODLOAD=OK || echo MODLOAD=FAIL",
                      cmd_timeout)
        if phase_token(out, "MODLOAD") != "OK":
            log("FAIL: could not load the module / create /dev/vms")
            return 14
        log("OK: module loaded and /dev/vms created")

        # =====================================================================
        # PROCTAB (collapsed): A ($SETPRN bg) launched, B ($GETJPI by name) and
        # C ($PROCESS_SCAN) query it, A killed -- all in one in-guest command,
        # asserted in-guest, one token. A blocks indefinitely (vmsproctab bg) so
        # both readers observe it before it is killed.
        # =====================================================================
        if skip_proctab_bg:
            # NEGATIVE CONTROL: A is never launched, so B's by-name $GETJPI must
            # find nothing. One token; the harness goes RED right here if B finds
            # a name A never registered (a CI step asserts the nonzero exit).
            negp = ("GJ=$(%s getjpi_name P4APROC1 2>&1); R=$?; "
                    "{ echo '### proctab-negctl getjpi'; echo \"$GJ\"; } >>%s; "
                    "{ [ $R -ne 0 ] && ! echo \"$GJ\" | "
                    "grep -q 'PROCTAB GETJPI_FOUND name=P4APROC1'; } "
                    "&& echo PROCTAB=NEGCTL_NOTFOUND || echo PROCTAB=NEGCTL_FOUND"
                    % (PT, TX))
            rc, out = run(child, negp, cmd_timeout)
            if phase_token(out, "PROCTAB") == "NEGCTL_FOUND":
                log("FAIL (negctl): B found a name process A never "
                    "registered -- the negative control has no teeth")
                return 33
            log("OK (negctl): B's GETJPI-by-name correctly found nothing "
                "when A never registered -- the positive assertion has teeth")
            return 30

        proc = ("rm -f /tmp/pt_a.out; "
                "%s bg P4APROC1 >/tmp/pt_a.out 2>&1 & AP=$!; sleep 2; "
                "GJ=$(%s getjpi_name P4APROC1 2>&1); "
                "PS=$(%s procscan_find P4APROC1 128 2>&1); "
                "kill $AP 2>/dev/null; wait 2>/dev/null; "
                "{ echo '### proctab A'; cat /tmp/pt_a.out; "
                "echo '### proctab B getjpi'; echo \"$GJ\"; "
                "echo '### proctab C procscan'; echo \"$PS\"; } >>%s; "
                "F=''; echo \"$GJ\" | "
                "grep -q 'PROCTAB GETJPI_FOUND name=P4APROC1' || F=\"$F getjpi\"; "
                "echo \"$PS\" | grep -q 'PROCTAB PROCSCAN_FOUND' || F=\"$F procscan\"; "
                "[ -z \"$F\" ] && echo PROCTAB=PASS || echo \"PROCTAB=FAIL:$F\""
                % (PT, PT, PT, TX))
        rc, out = run(child, proc, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "PROCTAB")
        if tok != "PASS":
            if tok and "getjpi" in tok:
                log("FAIL: process B (a DIFFERENT process) did not resolve "
                    "process A's name via $GETJPI")
                return 31
            log("FAIL: process C's $PROCESS_SCAN did not enumerate A's row "
                "(phase token: %s)" % tok)
            return 32
        log("OK: process B (a DIFFERENT process) resolved A's name via "
            "$GETJPI -- the process table is shared kernel state (INV-6)")
        log("OK: process C enumerated the shared process table with "
            "$PROCESS_SCAN and found A's row")

        # =====================================================================
        # MBX: A creates a TEMPORARY mailbox and holds it open; B writes; C
        # reads the same bytes. TEMPORARY, not permanent: on both substrates
        # $CREMBX(permanent=1) needs PRMMBX, which a default-privilege test
        # process does not hold (test_kmod_mbx.c asserts that SS$_NOPRIV
        # explicitly rather than dodging it) -- so the cross-process by-name
        # proof uses a temporary mailbox, exactly as Linux's own
        # test_syssvc_mbx_crossproc.c / test_kmod_mbx.c do. A stays alive
        # (create_hold) so the mailbox survives long enough for B and C to
        # reach it by name; it is freed when A's channel closes at the end of
        # the hold (vms_mbx_release_all on process exit).
        # =====================================================================
        mbx = ("rm -f /tmp/mb_a.out; "
               "%s create_hold 8 >/tmp/mb_a.out 2>&1 & AP=$!; sleep 2; "
               "D=$(grep 'MBX CREATE' /tmp/mb_a.out | "
               "sed 's/.*devnam=//; s/ .*//' | head -1); "
               "W=$(%s write \"$D\" 'P4A MBX HELLO' 2>&1); WR=$?; "
               "R=$(%s read \"$D\" 'P4A MBX HELLO' 2>&1); RR=$?; "
               "kill $AP 2>/dev/null; wait 2>/dev/null; "
               "{ echo '### mbx A'; cat /tmp/mb_a.out; echo \"### mbx devnam=$D\"; "
               "echo '### mbx B write'; echo \"$W\"; "
               "echo '### mbx C read'; echo \"$R\"; } >>%s; "
               "F=''; grep -q 'MBX CREATE' /tmp/mb_a.out || F=\"$F create\"; "
               "[ -n \"$D\" ] || F=\"$F devnam\"; "
               "{ [ $WR -eq 0 ] && echo \"$W\" | grep -q 'MBX WRITE'; } "
               "|| F=\"$F write\"; "
               "{ [ $RR -eq 0 ] && echo \"$R\" | grep -q 'match=1'; } "
               "|| F=\"$F read\"; "
               "[ -z \"$F\" ] && echo MBX=PASS || echo \"MBX=FAIL:$F\""
               % (MB, MB, MB, TX))
        rc, out = run(child, mbx, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "MBX")
        if tok != "PASS":
            log("FAIL: mailbox cross-process proof failed (phase token: %s)" % tok)
            if tok and ("create" in tok or "devnam" in tok):
                return 40
            if tok and "write" in tok:
                return 42
            return 43
        log("OK: process A created a temporary mailbox and B (a DIFFERENT "
            "process) wrote to it by name")
        log("OK: process C (a THIRD, different process) read back the EXACT "
            "bytes B wrote -- the mailbox message queue is shared kernel "
            "state (INV-6)")

        # =====================================================================
        # LOCK: A holds EX; B's ENQW blocks; A releases; B is granted.
        # =====================================================================
        lock = ("rm -f /tmp/lk_a.out /tmp/lk_b.out; F=''; "
                "%s hold_release P4ARES 5 8 >/tmp/lk_a.out 2>&1 & AP=$!; sleep 2; "
                "grep -q 'LOCK ENQ GRANTED' /tmp/lk_a.out || F=\"$F ahold\"; "
                "%s enqw P4ARES 5 >/tmp/lk_b.out 2>&1 & BP=$!; sleep 2; "
                "grep -q 'LOCK ENQW GRANTED' /tmp/lk_b.out && F=\"$F notblocked\"; "
                "i=0; while [ $i -lt %d ]; do "
                "grep -q 'LOCK DEQ RELEASED' /tmp/lk_a.out && break; "
                "i=$((i+1)); sleep 1; done; "
                "grep -q 'LOCK DEQ RELEASED' /tmp/lk_a.out || F=\"$F norelease\"; "
                "i=0; while [ $i -lt %d ]; do "
                "grep -q 'LOCK ENQW GRANTED' /tmp/lk_b.out && break; "
                "i=$((i+1)); sleep 1; done; "
                "grep -q 'LOCK ENQW GRANTED' /tmp/lk_b.out || F=\"$F nogrant\"; "
                "kill $AP $BP 2>/dev/null; wait 2>/dev/null; "
                "{ echo '### lock A'; cat /tmp/lk_a.out; "
                "echo '### lock B'; cat /tmp/lk_b.out; } >>%s; "
                "[ -z \"$F\" ] && echo LOCK=PASS || echo \"LOCK=FAIL:$F\""
                % (LK, LK, PB, PB, TX))
        rc, out = run(child, lock, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "LOCK")
        if tok != "PASS":
            log("FAIL: lock manager cross-process block/wake proof failed "
                "(phase token: %s)" % tok)
            if tok and "ahold" in tok:
                return 50
            if tok and "notblocked" in tok:
                return 51
            if tok and "norelease" in tok:
                return 52
            return 53
        log("OK: process A held an EXCLUSIVE lock, B's conflicting synchronous "
            "$ENQW BLOCKED IN THE KERNEL, then UNBLOCKED and was GRANTED the "
            "instant A released -- the lock resource database + wait queue "
            "are shared kernel state (INV-6, THE PAYOFF)")

        # =====================================================================
        # ACCESS: intra-process round trip, then A mutates privs; B observes.
        # =====================================================================
        acc = ("S=$(%s selftest 2>&1); SR=$?; "
               "rm -f /tmp/acc_a.out; "
               "%s setpriv_bg P4APRIV1 10 >/tmp/acc_a.out 2>&1 & AP=$!; sleep 2; "
               "G=$(%s getpriv P4APRIV1 2>&1); GR=$?; "
               "kill $AP 2>/dev/null; wait 2>/dev/null; "
               "{ echo '### access selftest'; echo \"$S\"; "
               "echo '### access A setpriv'; cat /tmp/acc_a.out; "
               "echo '### access B getpriv'; echo \"$G\"; } >>%s; "
               "F=''; "
               "{ [ $SR -eq 0 ] && echo \"$S\" | grep -q 'ACCESS SELFTEST PASS'; } "
               "|| F=\"$F selftest\"; "
               "{ [ $GR -eq 0 ] && echo \"$G\" | grep -q 'cmexec_clear=1'; } "
               "|| F=\"$F getpriv\"; "
               "[ -z \"$F\" ] && echo ACCESS=PASS || echo \"ACCESS=FAIL:$F\""
               % (AC, AC, AC, TX))
        rc, out = run(child, acc, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "ACCESS")
        if tok != "PASS":
            log("FAIL: access-mode cross-process proof failed (phase token: %s)"
                % tok)
            return 60 if (tok and "selftest" in tok) else 61
        log("OK: SETMODE/GETMODE/ENTER_IMAGE/DCLAST/DELIVERAST/IMAGE_RUNDOWN "
            "round-trip through the real /dev/vms")
        log("OK: process B (a DIFFERENT process) observed A's real $SETPRV "
            "mutation via $GETJPI -- the privilege mask is shared kernel "
            "state (INV-6)")

        # =====================================================================
        # AST: A arms a mailbox write-attention AST and $HIBERs; B's write on
        # that SAME mailbox fires the AST and wakes A -- cross-process.
        #
        # NOT collapsed into one in-guest command like the phases above: the
        # write-attention path is timing-sensitive (B's $QIO write must land
        # AFTER A is actually parked in $HIBER on the armed channel), and driving
        # it as discrete steps -- arm+HIBER, observe ARMED, then write, then
        # observe FIRED -- gives A the interval it needs; a single back-to-back
        # in-guest command raced the write ahead of the park and wedged. The
        # helper A is launched with a trailing `&' so the console driver treats it
        # as a non-retriable background launch (it must not be duplicated); the
        # remaining steps are idempotent polls/reads and retry to the deadline.
        # =====================================================================
        rc, _ = run(child,
                    "rm -f /tmp/ast_a.out; "
                    "%s wrtattn_bg P4AAST1 %d >/tmp/ast_a.out 2>&1 &"
                    % (AC, poll_budget), cmd_timeout)
        rc, out = run(child,
                      "i=0; while [ $i -lt %d ]; do "
                      "grep -q 'AST WRTATTN ARMED' /tmp/ast_a.out && break; "
                      "i=$((i+1)); sleep 1; done; cat /tmp/ast_a.out; "
                      "echo ---END---" % poll_budget, cmd_timeout)
        ast_devnam = parse_token(out, "DEVNAM")
        if "AST WRTATTN ARMED" not in out or not ast_devnam:
            log("FAIL: process A never armed its write-attention AST "
                "(bounded poll expired)")
            return 70
        if "AST WRTATTN FIRED" in out:
            log("FAIL: the AST fired before ANY process wrote to the "
                "mailbox -- the wait has no teeth (it did not actually block)")
            return 71

        rc, out = run(child, "%s wrtattn_write %s" % (AC, ast_devnam), cmd_timeout)
        if rc != 0 or "AST WRTATTN WRITE status=" not in out:
            log("FAIL: process B's write to A's mailbox failed")
            return 72

        rc, out = run(child,
                      "i=0; while [ $i -lt %d ]; do "
                      "grep -q 'AST WRTATTN FIRED' /tmp/ast_a.out && break; "
                      "i=$((i+1)); sleep 1; done; cat /tmp/ast_a.out; "
                      "echo ---END---" % poll_budget, cmd_timeout)
        if "AST WRTATTN FIRED astprm=0x" not in out or "match=1" not in out:
            log("FAIL: process A's $HIBER never woke / DELIVERAST did not "
                "return the write-attention AST B's write should have fired")
            return 73
        log("OK: process B's mailbox write WOKE process A's $HIBER and "
            "delivered the EXACT armed AST -- the AST queue + HIBER/wake "
            "path are shared kernel state reachable from a DIFFERENT "
            "process (INV-6, THE AST PAYOFF)")

        # ---- evidence: read the full in-guest transcript ONCE ---------------
        run(child, "echo '=== P4-A TRANSCRIPT ==='; cat %s; echo '=== END ==='"
            % TX, cmd_timeout)

        # ---- cleanup --------------------------------------------------------
        run(child, "modunload vms", cmd_timeout)
        try:
            a.halt()
        except Exception as e:
            log("note: halt raised (harmless): %s" % e)

        log("P4A: ALL CHECKS PASSED -- proctab, mbx, lock, access and ast, "
            "all compiled from src/kernel-core/ with the NetBSD backend, "
            "share real kernel state across processes on NetBSD/amd64 (INV-6). "
            "Combined with P2c's event-flag proof, every executive facility "
            "is now proven cross-process against a real /dev/vms.")
        return 0

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD console")
        log("  %s" % e)
        return 1
    except pexpect.EOF as e:
        log("FAIL: qemu exited unexpectedly (EOF on the console)")
        log("  %s" % e)
        return 1
    except Exception:
        log("FAIL: unexpected error driving the harness")
        traceback.print_exc()
        return 2
    finally:
        try:
            if child is not None and child.isalive():
                child.terminate(force=True)
        except Exception:
            pass


def _on_term(signum, frame):
    raise SystemExit(1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _on_term)
    sys.exit(main())
