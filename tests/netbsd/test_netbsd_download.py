#!/usr/bin/env python3
# Unit test for netbsd_download (rd vms-8a8). Run: python3 tests/netbsd/test_netbsd_download.py
# Uses a local HTTP server that truncates every response after CUT bytes, like
# the flaky origin did, to prove resume-by-Range completes and a 404 passes through.
import hashlib, http.server, os, sys, tempfile, threading
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netbsd_download as nd

DATA = os.urandom(3 * 1024 * 1024 + 123)
CUT = 700 * 1024


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/missing":
            self.send_error(404)
            return
        start = 0
        rng = self.headers.get("Range")
        if rng and getattr(H, "no_range", False):
            self.send_error(404)
            return
        if rng:
            start = int(rng.split("=")[1].rstrip("-"))
        body = DATA[start:]
        self.send_response(206 if rng else 200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body[:CUT])          # then drop the connection short
        self.wfile.flush()
        self.close_connection = True


def main():
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % srv.server_port
    q = lambda *_a: None
    dl = nd.make_download_file(sleep=q, logfn=q)
    d = tempfile.mkdtemp()
    out = os.path.join(d, "f")
    dl(base + "/big", out)
    assert hashlib.sha256(open(out, "rb").read()).digest() == hashlib.sha256(DATA).digest()
    assert not os.path.exists(out + ".part")
    print("PASS netbsd_download: truncating server -> resumed to a byte-exact file")
    try:
        dl(base + "/missing", os.path.join(d, "m"))
        raise SystemExit("404 did not raise")
    except IOError as e:
        assert "HTTP error code 404" in str(e) and not isinstance(e, RuntimeError)
    print("PASS netbsd_download: 404 passes through as IOError (extension probing)")
    # a server that makes NO progress -> RuntimeError (never cached as MISSING)
    def dead(req, timeout=None):
        raise OSError("connection reset")
    try:
        nd.make_download_file(opener=dead, max_stalls=3, sleep=q, logfn=q)(base + "/big", os.path.join(d, "z"))
        raise SystemExit("stall did not raise")
    except RuntimeError:
        pass
    print("PASS netbsd_download: persistent no-progress -> RuntimeError")
    # an edge that 404s every Range request must not make the file "missing":
    # restart from 0 each time, and with a truncating server end as a loud
    # RuntimeError, never an IOError that anita would cache as .MISSING.
    H.no_range = True
    try:
        nd.make_download_file(max_stalls=3, sleep=q, logfn=q)(base + "/big", os.path.join(d, "r"))
        raise SystemExit("range-404 did not raise")
    except RuntimeError:
        pass
    H.no_range = False
    print("PASS netbsd_download: Range-404 is not 'missing' (loud RuntimeError)")
    srv.shutdown()


main()
