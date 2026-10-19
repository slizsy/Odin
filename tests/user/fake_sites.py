# pretends to be a bunch of sites. "alice" exists everywhere, nobody else does
import http.server, time, sys
REAL = "alice"
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
        if kind == "echo":     return self.send(200, self.path.encode())
        self.send(404)
# odin fires a bunch of requests at once. python's default listen queue is 5, and macos
# refuses anything past that instead of waiting, so the test would see fake failures
class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128
    daemon_threads = True
Server(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
