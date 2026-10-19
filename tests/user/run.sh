#!/bin/sh
# spins up fake sites and makes sure odin judges every kind of response right
set -u
cd "$(dirname "$0")"
ODIN=${1:-../../odin}
PORT=$((20000 + $$ % 20000))
TMP=$(mktemp -d)
trap 'kill $SERVER 2>/dev/null; rm -rf "$TMP"' EXIT

PY=$(command -v python3 || command -v python) || { echo "need python3 for this test"; exit 1; }
"$PY" fake_sites.py "$PORT" & SERVER=$!
sed "s/PORT/$PORT/" sites.txt > "$TMP/sites.txt"

# don't let a proxy in the environment eat localhost requests
export NO_PROXY='*' no_proxy='*'

# wait for the fake server to come up (slow on some ci boxes)
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    "$PY" -c "import urllib.request as u; u.urlopen('http://127.0.0.1:$PORT/status/alice', timeout=1)" 2>/dev/null && break
    sleep 0.5
done
"$ODIN" user -j -t 2 --sites "$TMP/sites.txt" alice bob > "$TMP/out.json" || { echo "odin failed"; exit 1; }

"$PY" - "$TMP/out.json" <<'PY'
import json, sys
d = {u["username"]: {s["site"]: s["status"] for s in u["sites"]} for u in json.load(open(sys.argv[1]))}
want = {
    "alice": dict(StatusSite="found", AbsentSite="found", PresentSite="found", RedirectSite="found", SubdomainSite="found"),
    "bob":   dict(StatusSite="not_found", AbsentSite="not_found", PresentSite="not_found", RedirectSite="not_found", SubdomainSite="not_found"),
}
for u in ("alice", "bob"):
    want[u].update(GoneSite="not_found", EmptySite="not_found", BlockedSite="unknown", RateSite="unknown",
                   BoomSite="unknown", SlowSite="unknown", BigSite="unknown", CloudflareSite="unknown")
bad = [(u, s, d[u].get(s), w) for u in want for s, w in want[u].items() if d[u].get(s) != w]
for u, s, got, w in bad: print(f"  FAIL {u} @ {s}: got {got}, wanted {w}")
print(f"{sum(len(w) for w in want.values()) - len(bad)}/{sum(len(w) for w in want.values())} user checks passed")
sys.exit(1 if bad else 0)
PY
