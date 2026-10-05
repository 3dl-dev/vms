#!/usr/bin/env python3
# Unit test for fetch_verified.sh: a mirror that 404s, then a mirror that cuts
# every response short -> must resume to a byte-exact file; a wrong pin must fail.
# Run: python3 tools/cross-vax/test_fetch_verified.py
import hashlib, http.server, os, subprocess, tempfile, threading

HERE = os.path.dirname(os.path.abspath(__file__))
D = os.urandom(2_000_000)
SHA = hashlib.sha256(D).hexdigest()


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/bad":
            self.send_error(404)
            return
        st, rng = 0, self.headers.get("Range")
        if rng:
            st = int(rng.split("=")[1].rstrip("-"))
        body = D[st:]
        self.send_response(206 if rng else 200)
        if rng:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (st, len(D) - 1, len(D)))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body[:600_000])      # drop the connection short
        self.close_connection = True


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
base = "http://127.0.0.1:%d" % srv.server_port
d = tempfile.mkdtemp()
env = dict(os.environ, FETCH_BACKOFF="0")
f = os.path.join(d, "f")
r = subprocess.run(["sh", os.path.join(HERE, "fetch_verified.sh"), "sha256", SHA, f,
                    base + "/bad", base + "/x"], capture_output=True, text=True, env=env, timeout=60)
assert r.returncode == 0, r.stderr
assert hashlib.sha256(open(f, "rb").read()).hexdigest() == SHA
print("PASS fetch_verified: 404 mirror skipped, truncating mirror resumed to a verified file")
r = subprocess.run(["sh", os.path.join(HERE, "fetch_verified.sh"), "sha256", "0" * 64,
                    os.path.join(d, "g"), base + "/x"], capture_output=True, text=True,
                   env=dict(env, FETCH_ROUNDS="2"), timeout=60)
assert r.returncode != 0 and not os.path.exists(os.path.join(d, "g"))
print("PASS fetch_verified: wrong pin -> hard failure, no file left")
