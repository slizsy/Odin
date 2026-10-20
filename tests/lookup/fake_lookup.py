# fake DoH / RDAP / crt.sh / ipwho / gravatar server, so the domain/ip/email modules can be
# tested without touching the real internet. one process, dispatched by path.
import http.server, json, sys, hashlib
from urllib.parse import urlparse, parse_qs

# dns answers keyed by (lowercased name without trailing dot, type number)
A, AAAA, MX, NS, TXT, PTR, CAA = 1, 28, 15, 2, 16, 12, 257
ZONE = {
    ("example.test", A): ["93.184.216.34"],
    ("example.test", AAAA): ["2606:2800:220:1:248:1893:25c8:1946"],
    ("example.test", MX): ["10 mail.example.test."],
    ("example.test", NS): ["ns1.example.test.", "ns2.example.test."],
    ("example.test", TXT): ['"v=spf1 include:_spf.example.test ~all"', '"something-else=1"'],
    ("example.test", CAA): ['0 issue "letsencrypt.org"'],
    ("_dmarc.example.test", TXT): ['"v=DMARC1; p=reject; rua=mailto:dmarc@example.test"'],
    # email tests
    ("mailtest.test", MX): ["10 mx.mailtest.test."],
    ("nomx.test", A): ["203.0.113.9"],   # no MX but resolves -> "maybe"
    # void.test: nothing at all -> can't receive mail
    # ptr
    ("8.8.8.8.in-addr.arpa", PTR): ["dns.google."],
}

def doh(name, t):
    name = name.rstrip(".").lower()
    if name == "nxdomain.test":
        return {"Status": 3, "Answer": []}
    ans = [{"name": name + ".", "type": t, "TTL": 300, "data": d} for d in ZONE.get((name, t), [])]
    return {"Status": 0, "Answer": ans}

RDAP_DOMAIN = {
    "example.test": {
        "objectClassName": "domain", "ldhName": "EXAMPLE.TEST",
        "status": ["client transfer prohibited", "server delete prohibited"],
        "events": [
            {"eventAction": "registration", "eventDate": "1995-08-14T04:00:00Z"},
            {"eventAction": "expiration", "eventDate": "2030-08-13T04:00:00Z"},
            {"eventAction": "last changed", "eventDate": "2024-08-14T07:01:44Z"},
        ],
        "secureDNS": {"delegationSigned": True},
        "entities": [{"roles": ["registrar"],
                      "vcardArray": ["vcard", [["version", {}, "text", "4.0"],
                                               ["fn", {}, "text", "Example Registrar, Inc."]]]}],
        "nameservers": [{"ldhName": "ns1.example.test"}, {"ldhName": "ns2.example.test"}],
    }
}
RDAP_IP = {
    "8.8.8.8": {
        "objectClassName": "ip network", "handle": "NET-8-8-8-0-1", "name": "GOGL",
        "startAddress": "8.8.8.0", "endAddress": "8.8.8.255", "ipVersion": "v4",
        "type": "DIRECT ALLOCATION", "country": "US",
        "cidr0_cidrs": [{"v4prefix": "8.8.8.0", "length": 24}],
        "entities": [{"roles": ["registrant"],
                      "vcardArray": ["vcard", [["version", {}, "text", "4.0"], ["fn", {}, "text", "Google LLC"]]],
                      "entities": [{"roles": ["abuse"],
                                    "vcardArray": ["vcard", [["version", {}, "text", "4.0"],
                                                             ["fn", {}, "text", "Abuse"],
                                                             ["email", {}, "text", "network-abuse@google.com"]]]}]}],
    }
}
IPWHO = {
    "8.8.8.8": {"ip": "8.8.8.8", "success": True, "type": "IPv4", "country": "United States",
                "region": "California", "city": "Mountain View",
                "connection": {"asn": 15169, "org": "Google LLC", "isp": "Google LLC", "domain": "google.com"},
                "timezone": {"id": "America/Los_Angeles"}},
}
# crt.sh style rows: dupes, wildcards and a stray unrelated name to exercise filtering/dedup
CRTSH = {
    "example.test": [
        {"name_value": "example.test\nwww.example.test"},
        {"name_value": "*.example.test"},
        {"name_value": "www.example.test"},          # dup
        {"name_value": "api.example.test\nblog.example.test"},
        {"name_value": "notours.example.org"},       # filtered out
    ]
}
# gravatar: the test email, hashed the way the module does (sha256 of the lowercased address)
GRAV_EMAIL = "grav@example.test"
GRAV_HASH = hashlib.sha256(GRAV_EMAIL.encode()).hexdigest()
GRAV = {"entry": [{"hash": GRAV_HASH, "displayName": "Grav Person",
                   "profileUrl": "https://gravatar.com/gravperson"}]}


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        path = u.path
        q = parse_qs(u.query)
        if path == "/dns-query":
            name = q.get("name", [""])[0]
            t = int(q.get("type", ["1"])[0])
            return self.send_json(200, doh(name, t))
        if path.startswith("/domain/"):
            d = path[len("/domain/"):].lower()
            return self.send_json(200, RDAP_DOMAIN[d]) if d in RDAP_DOMAIN else self.send_json(404, {})
        if path.startswith("/ip/"):
            ip = path[len("/ip/"):]
            return self.send_json(200, RDAP_IP[ip]) if ip in RDAP_IP else self.send_json(404, {})
        if path.startswith("/ipwho/"):
            ip = path[len("/ipwho/"):]
            if ip in IPWHO:
                return self.send_json(200, IPWHO[ip])
            return self.send_json(200, {"ip": ip, "success": False, "message": "no"})
        if path.startswith("/gravatar/"):
            h = path[len("/gravatar/"):].replace(".json", "")
            return self.send_json(200, GRAV) if h == GRAV_HASH else self.send_json(404, {})
        if path == "/" and "q" in q:   # crt.sh
            qv = q["q"][0]             # like %.example.test
            d = qv.lstrip("%.").lower()
            return self.send_json(200, CRTSH.get(d, []))
        self.send_json(404, {})


if __name__ == "__main__":
    port = int(sys.argv[1])
    http.server.HTTPServer.request_queue_size = 128
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
    srv.handle_error = lambda *a: None
    srv.serve_forever()
