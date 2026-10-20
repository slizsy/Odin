#!/bin/sh
# points the domain/ip/email modules at the fake lookup server and checks what they report
set -u
cd "$(dirname "$0")"
ODIN=${1:-../../odin}
PORT=$((23000 + $$ % 20000))
TMP=$(mktemp -d)
trap 'kill $SERVER 2>/dev/null; rm -rf "$TMP"' EXIT

PY=$(command -v python3 || command -v python) || { echo "need python3 for this test"; exit 1; }
"$PY" fake_lookup.py "$PORT" & SERVER=$!

export NO_PROXY='*' no_proxy='*'
export ODIN_DOH_URL="http://127.0.0.1:$PORT/dns-query"
export ODIN_RDAP_URL="http://127.0.0.1:$PORT"
export ODIN_CRTSH_URL="http://127.0.0.1:$PORT"
export ODIN_IPINFO_URL="http://127.0.0.1:$PORT/ipwho"
export ODIN_GRAVATAR_URL="http://127.0.0.1:$PORT/gravatar"

# wait for it
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    "$PY" -c "import urllib.request as u; u.urlopen('http://127.0.0.1:$PORT/ip/8.8.8.8', timeout=1)" 2>/dev/null && break
    sleep 0.5
done

"$ODIN" domain -j example.test       > "$TMP/dom.json"   || { echo "odin domain failed"; exit 1; }
"$ODIN" ip -j 8.8.8.8                 > "$TMP/ip.json"    || { echo "odin ip failed"; exit 1; }
"$ODIN" email -j grav@example.test    > "$TMP/e1.json"   || true
"$ODIN" email -j admin@mailtest.test  > "$TMP/e2.json"   || true
"$ODIN" email -j user@void.test       > "$TMP/e3.json"   || true

"$PY" - "$TMP" <<'PY'
import json, sys
tmp = sys.argv[1]
bad = total = 0
def check(cond, msg):
    global bad, total
    total += 1
    if not cond:
        bad += 1
        print("  FAIL", msg)

dom = json.load(open(tmp + "/dom.json"))
check(dom["dns"]["a"] == ["93.184.216.34"], "domain A record")
check("2606:2800:220:1:248:1893:25c8:1946" in dom["dns"]["aaaa"], "domain AAAA record")
check(dom["dns"]["mx"] == ["10 mail.example.test."], "domain MX record")
check(sorted(dom["dns"]["ns"]) == ["ns1.example.test.", "ns2.example.test."], "domain NS records")
check(dom["dns"]["spf"].startswith("v=spf1"), "domain SPF extracted")
check(dom["dns"]["dmarc"].startswith("v=DMARC1"), "domain DMARC extracted")
check(dom["registration"]["registrar"] == "Example Registrar, Inc.", "domain registrar")
check(dom["registration"]["created"].startswith("1995"), "domain created date")
check(dom["registration"]["expires"].startswith("2030"), "domain expiry date")
check(dom["registration"]["dnssec"] == "signed", "domain dnssec")
subs = set(dom["subdomains"])
check(subs == {"example.test", "www.example.test", "api.example.test", "blog.example.test"},
      "subdomains deduped+filtered, got %r" % sorted(subs))
check("notours.example.org" not in subs, "crt.sh unrelated name filtered")

ip = json.load(open(tmp + "/ip.json"))
check(ip["ptr"] == "dns.google.", "ip reverse dns")
check(ip["network"]["name"] == "GOGL", "ip rdap net name")
check(ip["network"]["cidr"] == "8.8.8.0/24", "ip rdap cidr")
check(ip["network"]["org"] == "Google LLC", "ip rdap org")
check(ip["network"]["abuse"] == "network-abuse@google.com", "ip rdap abuse (nested entity)")
check(ip["geo"]["country"] == "United States", "ip geo country")
check(ip["geo"]["asn"] == "AS15169", "ip geo asn")
check(ip["geo"]["asn_org"] == "Google LLC", "ip geo asn org")

e1 = json.load(open(tmp + "/e1.json"))
check(e1["valid_syntax"] is True, "email valid syntax")
check(e1["deliverable"] is True and e1["mx_records"] == 1, "email example.test deliverable via MX")
check(e1["gravatar"] is True and e1["gravatar_name"] == "Grav Person", "email gravatar profile found")
e2 = json.load(open(tmp + "/e2.json"))
check(e2["mx_records"] == 1 and e2["deliverable"] is True, "email mailtest.test deliverable via MX")
check(e2["role_account"] is True, "email admin@ is a role account")
e3 = json.load(open(tmp + "/e3.json"))
check(e3["deliverable"] is False, "email void.test not deliverable")

print("lookup: %d/%d checks passed" % (total - bad, total))
sys.exit(1 if bad else 0)
PY