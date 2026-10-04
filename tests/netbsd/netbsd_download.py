"""Resumable downloads for anita (rd vms-8a8 proof-run finding).

anita 2.18 on Python < 3.13 fetches release media with FancyURLopener, which
raises a plain IOError for EVERY failure -- a truncated transfer looks like a
404. For an optional file download_if_missing_2() then prints "missing but
optional" and caches a .MISSING marker, so a flaky cdn.netbsd.org (observed
2026-10-04 from GitHub runners: `curl: (18) transfer closed with 27068897 bytes
remaining', and `retrieval incomplete: got only 8388608 out of 321972224
bytes' on EVERY whole-file retry -- the origin cuts long transfers) fails the
whole job with a misleading message, and restarting from byte 0 never gets past
the cut.

install() replaces anita.download_file with one that RESUMES via HTTP Range:
bytes already received are kept in <file>.part and only the remainder is
requested, so each attempt makes progress. A genuine 404 raises
IOError('HTTP error code 404') at once (anita's .tgz/.tar.xz extension probing
relies on it). If attempts stop making progress the failure is raised as a
RuntimeError -- NOT an IOError -- so it fails loudly with the real cause
instead of being cached as "missing".
"""
import http.client
import os
import time
import urllib.error
import urllib.request

_MAX_STALLS = 6          # consecutive attempts with zero new bytes
_CHUNK = 1 << 20


def _fetch_from(url, part, offset, opener):
    """Append url[offset:] to `part`. Returns total length if known else None."""
    req = urllib.request.Request(url)
    if offset:
        req.add_header("Range", "bytes=%d-" % offset)
    try:
        resp = opener(req, timeout=60)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            if offset:
                # Range on a file that exists: an edge that cannot serve the
                # range (observed: 404 for any offset > 0 while offset 0 is
                # 200) is not "the file is missing". Restart from byte 0.
                open(part, "wb").close()
                raise OSError("range request not served (404 at offset %d)" % offset)
            raise IOError("HTTP error code 404")
        if e.code == 416:                    # range past EOF: already complete
            return offset
        raise
    status = getattr(resp, "status", 200)
    if offset and status != 206:             # server ignored Range: start over
        offset = 0
        open(part, "wb").close()
    clen = resp.headers.get("Content-Length")
    total = (offset + int(clen)) if clen is not None else None
    with open(part, "ab") as f:
        while True:
            buf = resp.read(_CHUNK)
            if not buf:
                break
            f.write(buf)
    return total


def make_download_file(opener=urllib.request.urlopen, max_stalls=_MAX_STALLS,
                       sleep=time.sleep, logfn=print):
    def download_file(url, file):
        part = file + ".part"
        stalls = 0
        high = 0          # most bytes ever held; progress = beating it
        last = None
        while True:
            have = os.path.getsize(part) if os.path.exists(part) else 0
            try:
                total = _fetch_from(url, part, have, opener)
                got = os.path.getsize(part)
                if total is None or got == total:
                    os.rename(part, file)
                    return
                last = IOError("incomplete: %d of %d bytes" % (got, total))
            except IOError as e:
                if "HTTP error code 404" in str(e):
                    if os.path.exists(part):
                        os.unlink(part)
                    raise
                last = e
            except (OSError, http.client.HTTPException) as e:  # URLError, timeouts, resets
                last = e
            got = os.path.getsize(part) if os.path.exists(part) else 0
            stalls = 0 if got > high else stalls + 1
            high = max(high, got)
            logfn("download of %s interrupted at %d bytes (%s: %s), stall %d/%d"
                  % (url, got, type(last).__name__, last, stalls, max_stalls))
            if stalls >= max_stalls:
                raise RuntimeError("download of %s failed: no progress after %d "
                                   "attempts: %s" % (url, stalls, last))
            sleep(5)
    return download_file


def install(anita_module):
    anita_module.download_file = make_download_file()
