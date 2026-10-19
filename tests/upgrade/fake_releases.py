# pretends to be github's releases api so `odin upgrade` can be tested offline.
# usage: fake_releases.py PORT path/to/binary-to-serve
import hashlib, http.server, json, sys

PORT, BIN = int(sys.argv[1]), open(sys.argv[2], "rb").read()
ASSETS = ["odin-linux-x86_64", "odin-linux-arm64", "odin-macos-universal", "odin-windows-x86_64.exe"]
HTML = b"<html><body>" + b"oops, an error page, not a program " * 4000 + b"</body></html>"
base = f"http://127.0.0.1:{PORT}"

def sums(data, wrong=False):
    h = hashlib.sha256(data).hexdigest()
    if wrong: h = "0" * 64
    return "".join(f"{h}  {a}\n" for a in ASSETS).encode()

def release(tag, bin_path="bin", sums_path="sums", with_bin=True):
    assets = [{"name": a, "url": f"{base}/asset/{bin_path}"} for a in ASSETS] if with_bin else []
    assets.append({"name": "SHA256SUMS", "url": f"{base}/asset/{sums_path}"})
    return json.dumps({"tag_name": tag, "assets": assets}).encode()

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/octet-stream"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = self.path
        if p.startswith("/asset/"):
            # like github: without this accept header you get json about the file, not the file
            if self.headers.get("Accept") != "application/octet-stream":
                return self.send(200, b'{"name":"metadata, not the file"}', "application/json")
            what = p[len("/asset/"):]
            if what == "bin": return self.send(200, BIN)
            if what == "html": return self.send(200, HTML)
            if what == "sums": return self.send(200, sums(BIN))
            if what == "badsums": return self.send(200, sums(BIN, wrong=True))
            if what == "htmlsums": return self.send(200, sums(HTML))
            return self.send(404, b"")
        routes = {
            "/api/new": lambda: release("v99.0.0"),
            "/api/old": lambda: release("v0.0.1"),
            "/api/badsum": lambda: release("v99.0.0", sums_path="badsums"),
            "/api/html": lambda: release("v99.0.0", bin_path="html", sums_path="htmlsums"),
            "/api/nobin": lambda: release("v99.0.0", with_bin=False),
        }
        if p == "/api/private":
            if self.headers.get("Authorization") != "Bearer letmein": return self.send(404, b'{"message":"Not Found"}')
            return self.send(200, release("v99.0.0"), "application/json")
        if p in routes: return self.send(200, routes[p](), "application/json")
        self.send(404, b'{"message":"Not Found"}', "application/json")

class S(http.server.ThreadingHTTPServer):
    request_queue_size = 64
    daemon_threads = True
S(("127.0.0.1", PORT), H).serve_forever()
