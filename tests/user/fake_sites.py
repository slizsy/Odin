# pretends to be a bunch of sites. "alice" exists everywhere, nobody else does
import http.server, time, sys, json, threading
REAL = "alice"
attempts = {}  # (endpoint, name) -> how many times we've been asked, for the flaky ones
lock = threading.Lock()

def attempt(kind, name):
    with lock:
        attempts[(kind, name)] = attempts.get((kind, name), 0) + 1
        return attempts[(kind, name)]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body=b""):
        self.send_response(code); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        host = self.headers.get("Host", "").split(":")[0]
        parts = self.path.strip("/").split("/")
        kind, name = parts[0], (parts[1] if len(parts) > 1 else "")
        if host.endswith(".localhost") and host != "localhost":   # subdomain site, {}.localhost
            return self.send(200 if host.split(".")[0] == REAL else 404, b"blog")
        if kind == "status":   return self.send(200 if name == REAL else 404, b"profile")
        if kind == "absent":   return self.send(200, b"hi alice" if name == REAL else b"<h1>Sorry, no such user</h1>")
        if kind == "present":  return self.send(200, b'{"user":{"id":1}}' if name == REAL else b'{"user":null}')
        if kind == "blocked":  return self.send(403, b"go away bot")
        if kind == "ratelimit":return self.send(429)
        if kind == "boom":     return self.send(503)
        if kind == "gone":     return self.send(410)
        if kind == "empty":    return self.send(204)
        if kind == "slow":     time.sleep(4); return self.send(200)
        if kind == "redirect":
            self.send_response(302); self.send_header("Location", "/status/" + name); self.send_header("Content-Length", "0"); self.end_headers(); return
        if kind == "big":      # 3MB of junk, the "not found" marker is past our 8MB cap
            return self.send(200, b"x" * (9 << 20) + (b"" if name == REAL else b"NOPE"))
        if kind == "gap":      # just over 8MB, a pause, then the marker in its own tiny chunk.
            # catches odin gluing a late chunk onto a page it already cut off
            body = b"x" * ((8 << 20) + 100)
            self.send_response(200); self.send_header("Content-Length", str(len(body) + 4)); self.end_headers()
            self.wfile.write(body); self.wfile.flush(); time.sleep(0.3)
            if name != REAL: self.wfile.write(b"NOPE")
            else: self.wfile.write(b"yeah")
            return
        if kind == "cloudflare": return self.send(200, b"<html><title>Just a moment...</title>checking your browser</html>")
        if kind == "flaky429":   # rate limits the first try, says come back in 1s
            if attempt(kind, name) == 1:
                self.send_response(429); self.send_header("Retry-After", "1"); self.send_header("Content-Length", "0"); self.end_headers(); return
            return self.send(200 if name == REAL else 404, b"ok")
        if kind == "flaky503":   # falls over once, then fine
            if attempt(kind, name) == 1: return self.send(503, b"down")
            return self.send(200 if name == REAL else 404, b"ok")
        if kind == "longwait":   # wants us gone for 2 minutes, not worth waiting
            self.send_response(429); self.send_header("Retry-After", "120"); self.send_header("Content-Length", "0"); self.end_headers(); return
        if kind == "headers":    # echoes what we sent, lowercased
            return self.send(200, "\n".join(f"{k.lower()}: {v}" for k, v in self.headers.items()).encode())
        if kind == "strict":     # whatsmyname style: alice = real page, bob = proper 404, anyone else = something odd
            if name == REAL: return self.send(200, b"<div class=profile-header>alice</div>")
            if name == "bob": return self.send(404, b"<title>Nope</title>")
            return self.send(200, b"<html>maintenance page</html>")
        if kind == "slowban":    # a site-specific 'calm down' page served as a 200
            return self.send(200, b"<p>Please wait a few minutes before you try again.</p>")
        if kind == "echo":     return self.send(200, self.path.encode())
        self.send(404)
# odin fires a bunch of requests at once. python's default listen queue is 5, and macos
# refuses anything past that instead of waiting, so the test would see fake failures
def do_POST(self):
    # a json api: {"user": "alice"} -> {"exists": true}
    n = int(self.headers.get("Content-Length", 0))
    try: user = json.loads(self.rfile.read(n)).get("user")
    except Exception: return self.send(400, b"bad json")
    if self.path != "/api/lookup": return self.send(404, b"")
    if self.headers.get("Content-Type") != "application/json": return self.send(415, b"wrong content type")
    return self.send(200, b'{"exists":true}' if user == REAL else b'{"exists":false}')
H.do_POST = do_POST

class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128
    daemon_threads = True
    def handle_error(self, request, addr): pass  # odin hanging up on the slow pages is expected
Server(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
