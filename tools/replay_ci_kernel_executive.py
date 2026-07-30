#!/usr/bin/env python3
"""
replay_ci_kernel_executive.py - run the ci.yml kernel-executive assertions
VERBATIM against a captured harness run, without a GitHub runner.

WHY THIS EXISTS: .github/workflows/ci.yml only ever executes on push to main
or on a PR against main, so the assertion logic in the kernel-executive and
kernel-executive-negative-control jobs cannot be exercised while developing a
branch -- and a previous round of vms-1d9 shipped assertion blocks whose
weaknesses were only found later, by an adversary who re-derived them by hand.
Re-typing them by hand is exactly how a local "proof" drifts from what CI
actually runs.

This extracts the `run:` block of the named job's assertion step STRAIGHT OUT
of .github/workflows/ci.yml, substitutes only the container invocation (the
one line that must differ, since the run has already happened and is on disk),
and executes the rest with bash. If the YAML and the local check ever
disagree, this script is wrong by construction rather than silently stale.

Usage:
    tools/replay_ci_kernel_executive.py <positive|negative> <captured-output-file> [container-rc]
Exit code is the assertion block's own exit code.
"""
import re
import subprocess
import sys
import pathlib

JOBS = {
    "positive": "Boot QEMU, insmod vms.ko, run executive assertions against /dev/vms",
    "negative": "Assert the harness FAILS for the RIGHT REASON when the executive is absent",
}


def extract_run_block(ci_yml: str, step_name: str) -> str:
    lines = ci_yml.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == f"- name: {step_name}":
            break
    else:
        raise SystemExit(f"step not found in ci.yml: {step_name}")

    # Find the `run: |` that belongs to this step.
    for j in range(i + 1, len(lines)):
        if re.match(r"^\s*run: \|\s*$", lines[j]):
            break
        if re.match(r"^\s*- name: ", lines[j]):
            raise SystemExit(f"no 'run: |' block for step: {step_name}")
    else:
        raise SystemExit(f"no 'run: |' block for step: {step_name}")

    body_indent = len(lines[j + 1]) - len(lines[j + 1].lstrip())
    body = []
    for line in lines[j + 1:]:
        if line.strip() and (len(line) - len(line.lstrip())) < body_indent:
            break
        body.append(line[body_indent:] if line.strip() else "")
    return "\n".join(body)


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    which, capture = sys.argv[1], sys.argv[2]
    rc = sys.argv[3] if len(sys.argv) > 3 else "0"

    repo = pathlib.Path(__file__).resolve().parent.parent
    ci_yml = (repo / ".github/workflows/ci.yml").read_text()
    block = extract_run_block(ci_yml, JOBS[which])

    # The ONLY substitution: the container has already been run; read its
    # captured output and exit status instead of launching it again.
    block = re.sub(
        r"RAW=\$\(docker run --rm \S+ 2>&1\)\n\s*RC=\$\?",
        f'RAW=$(cat "{capture}")\n          RC={rc}',
        block,
    )
    if "RAW=$(cat " not in block:
        raise SystemExit("failed to substitute the docker-run line; ci.yml shape changed")

    env = {"GITHUB_WORKSPACE": str(repo), "PATH": "/usr/bin:/bin:/usr/sbin:/sbin"}
    proc = subprocess.run(["bash", "-e", "-c", block], env=env)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
