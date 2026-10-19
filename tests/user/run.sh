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
for f in sites.txt sites.json wmn-sites.json; do sed "s/PORT/$PORT/g" "$f" > "$TMP/$f"; done
"$ODIN" user -j -t 2 --sites "$TMP/sites.txt" alice bob > "$TMP/out.json" || { echo "odin failed (text)"; exit 1; }
"$ODIN" user -j -t 2 --sites "$TMP/sites.json" alice bob carol carol_9 al.ice > "$TMP/out2.json" || { echo "odin failed (json)"; exit 1; }
"$ODIN" user -j -t 2 --sites "$TMP/wmn-sites.json" alice bob carol > "$TMP/out3.json" || { echo "odin failed (wmn)"; exit 1; }

"$PY" - "$TMP/out.json" "$TMP/out2.json" "$TMP/out3.json" <<'PY'
import json, sys
def load(path):
    raw = json.load(open(path))
    return ({u["username"]: {s["site"]: s["status"] for s in u["sites"]} for u in raw},
            {(u["username"], s["site"]): s["detail"] for u in raw for s in u["sites"]})
want_text = {
    "alice": dict(StatusSite="found", AbsentSite="found", PresentSite="found", RedirectSite="found", SubdomainSite="found"),
    "bob":   dict(StatusSite="not_found", AbsentSite="not_found", PresentSite="not_found", RedirectSite="not_found", SubdomainSite="not_found"),
}
for u in ("alice", "bob"):
    want_text[u].update(GoneSite="not_found", EmptySite="not_found", BlockedSite="unknown", RateSite="unknown",
                        BoomSite="unknown", SlowSite="unknown", BigSite="unknown", GapSite="unknown", CloudflareSite="unknown")
want_json = {
    "alice": dict(Flaky429="found", Flaky503="found", LongWait="unknown", Headers="found", BrowserLike="found",
                  PostApi="found", Strict="found", SlowBan="unknown", LettersOnly="unknown", StripDots="found"),
    "bob": dict(Flaky429="not_found", Flaky503="not_found", PostApi="not_found", Strict="not_found"),
    "carol": dict(Strict="unknown"),                    # odd page = couldn't check, not a guess
    "carol_9": dict(LettersOnly="not_found"),           # regex says no, never asked the (broken) site
    "al.ice": dict(StripDots="found"),                  # dot stripped -> alice
}
want_wmn = {"alice": dict(WmnStrict="found", WmnPost="found"), "bob": dict(WmnStrict="not_found", WmnPost="not_found"),
            "carol": dict(WmnStrict="unknown")}
total = bad = 0
details = {}
for path, want in ((sys.argv[1], want_text), (sys.argv[2], want_json), (sys.argv[3], want_wmn)):
    got, why = load(path)
    details.update(why)
    for u, sites in want.items():
        for s, w in sites.items():
            total += 1
            if got.get(u, {}).get(s) != w:
                bad += 1
                print(f"  FAIL {u} @ {s}: got {got.get(u, {}).get(s)}, wanted {w} ({why.get((u, s)) or 'no detail'})")
    if path == sys.argv[3] and "WmnDead" in got["alice"]:
        bad += 1; total += 1; print("  FAIL WmnDead (valid: false) should have been skipped")
# the reasons matter too
for key, part in {("alice", "LongWait"): "wait 120s", ("alice", "SlowBan"): "Rate limited",
                  ("carol_9", "LettersOnly"): "not allowed", ("carol", "Strict"): "unexpected"}.items():
    total += 1
    if part not in (details.get(key) or ""):
        bad += 1; print(f"  FAIL detail for {key}: {details.get(key)!r} should mention {part!r}")
print(f"{total - bad}/{total} user checks passed")
sys.exit(1 if bad else 0)
PY
