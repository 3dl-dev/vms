#!/usr/bin/env python3
# Unit test for netbsd_download (rd vms-8a8). Run: python3 tests/netbsd/test_netbsd_download.py
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netbsd_download as nd


def flaky(fails, exc):
    calls = []
    def orig(url, f):
        calls.append(url)
        if len(calls) <= fails:
            raise exc
        return "ok"
    return orig, calls


def main():
    q = lambda *_a: None
    # transient failures are retried until success
    o, c = flaky(3, IOError("retrieval incomplete: got only 1 out of 2 bytes"))
    assert nd.make_download_file(o, sleep=q, logfn=q)("u", "f") == "ok" and len(c) == 4
    # a real 404 is passed through immediately (extension probing relies on it)
    o, c = flaky(99, IOError("HTTP error code 404"))
    try:
        nd.make_download_file(o, sleep=q, logfn=q)("u", "f"); raise SystemExit("no raise")
    except IOError:
        assert len(c) == 1
    # persistent failure -> RuntimeError (not IOError, so never cached as MISSING)
    o, c = flaky(99, IOError("connection reset"))
    try:
        nd.make_download_file(o, attempts=3, sleep=q, logfn=q)("u", "f"); raise SystemExit("no raise")
    except RuntimeError:
        assert len(c) == 3
    print("PASS netbsd_download: retry/404/persistent cases")


main()
