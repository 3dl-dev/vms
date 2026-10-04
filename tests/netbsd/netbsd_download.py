"""Retrying downloads for anita (rd vms-8a8 proof-run finding).

anita 2.18 on Python < 3.13 fetches release media with FancyURLopener, which
raises a plain IOError('HTTP error code N') for EVERY failure -- a truncated
transfer or a connection reset looks identical to a 404. For an optional file
download_if_missing_2() then prints "missing but optional" and persists a
.MISSING marker, so a transient cdn.netbsd.org hiccup (observed 2026-10-04:
`curl: (18) transfer closed with 27068897 bytes remaining', and install sets
`base'/`comp' reported "does not exist with extension .tgz nor .tar.xz" while
the files are 200 OK) fails the whole job with a misleading message.

install() wraps anita.download_file: a genuine 404 is passed through at once
(the .tgz/.tar.xz extension probing relies on it); any other failure is retried
with backoff and, if it persists, raised as a RuntimeError -- NOT an IOError --
so it fails loudly with the real cause instead of being cached as "missing".
"""
import time

_ATTEMPTS = 5


def _is_404(exc):
    return "HTTP error code 404" in str(exc)


def make_download_file(orig, attempts=_ATTEMPTS, sleep=time.sleep, logfn=print):
    def download_file(url, file):
        last = None
        for n in range(1, attempts + 1):
            try:
                return orig(url, file)
            except IOError as e:          # urllib errors are IOError/OSError
                if _is_404(e):
                    raise
                last = e
                logfn("download of %s failed (%s: %s), attempt %d/%d"
                      % (url, type(e).__name__, e, n, attempts))
                if n < attempts:
                    sleep(5 * n)
        raise RuntimeError("download of %s failed after %d attempts: %s"
                           % (url, attempts, last))
    return download_file


def install(anita_module):
    anita_module.download_file = make_download_file(anita_module.download_file)
