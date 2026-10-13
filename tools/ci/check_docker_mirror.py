#!/usr/bin/env python3
"""check_docker_mirror.py - every CI job that pulls through the docker daemon
first points the daemon at the registry mirror.

Anonymous Docker Hub pulls from shared GitHub runner IPs fail intermittently
(rate limit 429, 502 Bad Gateway, auth errors -- rd vms-9a6, vms-d6c; a 502 on
`docker run docker.io/library/alpine:3.20` failed merge-queue run 38319510194).
.github/actions/docker-mirror makes the daemon try mirror.gcr.io first, and
.github/actions/setup-buildx does that too. A job whose run steps call
`docker run`, `docker pull`, `docker build` or `docker create` must use one of
them in an EARLIER step.

usage: check_docker_mirror.py [repo-root]   (exit 0 clean, 1 found)
       check_docker_mirror.py --selftest
"""
import glob
import os
import re
import sys

import yaml

DOCKER = re.compile(r'(?:^|[\s;&|(])docker\s+(?:run|pull|build|create)\b')
MIRRORS = ('./.github/actions/docker-mirror', './.github/actions/setup-buildx')


def job_findings(wf_name, jobs):
    out = []
    for jid, job in (jobs or {}).items():
        mirrored = False
        for i, step in enumerate(job.get('steps') or []):
            uses = str(step.get('uses') or '')
            if any(uses.startswith(m) for m in MIRRORS):
                mirrored = True
                continue
            run = str(step.get('run') or '')
            if not mirrored and DOCKER.search(run):
                out.append('%s: job %s step %d (%s) uses the docker daemon before '
                           'any docker-mirror/setup-buildx step'
                           % (wf_name, jid, i + 1, step.get('name') or uses or 'run'))
                break
    return out


def scan(root):
    hits = []
    for f in sorted(glob.glob(os.path.join(root, '.github/workflows/*.yml'))):
        doc = yaml.safe_load(open(f)) or {}
        hits += job_findings(os.path.basename(f), doc.get('jobs'))
    return hits


def selftest():
    good = {'j': {'steps': [{'uses': 'actions/checkout@v4'},
                            {'uses': './.github/actions/docker-mirror'},
                            {'run': 'docker run --rm docker.io/library/alpine:3.20 true'}]}}
    good2 = {'j': {'steps': [{'uses': './.github/actions/setup-buildx'},
                             {'run': 'docker build -t x .'}]}}
    bad = {'j': {'steps': [{'uses': 'actions/checkout@v4'},
                           {'run': 'set -e\n  docker run --rm alpine:3.20 true'}]}}
    late = {'j': {'steps': [{'run': 'docker pull alpine'},
                            {'uses': './.github/actions/docker-mirror'}]}}
    none = {'j': {'steps': [{'run': 'echo "docker is not run here"'}]}}
    cases = [(good, 0), (good2, 0), (bad, 1), (late, 1), (none, 0)]
    fail = 0
    for jobs, want in cases:
        got = len(job_findings('case', jobs))
        if got != want:
            print('SELFTEST FAIL: %r -> %d, want %d' % (jobs, got, want))
            fail = 1
    print('selftest: %s' % ('FAIL' if fail else 'ok'))
    return fail


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--selftest':
        return selftest()
    hits = scan(sys.argv[1] if len(sys.argv) > 1 else '.')
    for h in hits:
        print('UNMIRRORED DOCKER: ' + h)
    if hits:
        print('FAIL: %d job(s) pull through the docker daemon without the registry mirror '
              '-- add `- uses: ./.github/actions/docker-mirror` before the step' % len(hits))
        return 1
    print('ok: every job that uses the docker daemon points it at the registry mirror first')
    return 0


if __name__ == '__main__':
    sys.exit(main())
