#!/bin/sh
# checks `odin upgrade` against a fake github: happy path, bad checksum, html instead of a
# binary, private repo tokens, read-only installs. the binary being upgraded is a throwaway copy
set -u
cd "$(dirname "$0")"
ODIN=${1:-../../odin}
case "$ODIN" in *.exe) EXT=.exe ;; *) EXT= ;; esac
PORT=$((20000 + ($$ + 777) % 20000))
TMP=$(mktemp -d)
trap 'kill $SERVER 2>/dev/null; chmod -R u+w "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT

# the "new release": a copy of odin with a marker on the end, still a working program
cp "$ODIN" "$TMP/served$EXT" && printf 'ODIN-UPGRADE-TEST' >> "$TMP/served$EXT"
PY=$(command -v python3 || command -v python) || { echo "need python3 for this test"; exit 1; }
"$PY" fake_releases.py "$PORT" "$TMP/served$EXT" & SERVER=$!
export NO_PROXY='*' no_proxy='*'
unset GITHUB_TOKEN GH_TOKEN
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    "$PY" -c "import urllib.request as u; u.urlopen('http://127.0.0.1:$PORT/api/new', timeout=1)" 2>/dev/null && break
    sleep 0.5
done

pass=0 fail=0
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "  FAIL $1"; }

# runs a fresh copy of odin against one fake endpoint. sets $out and $code
try() {
    mkdir -p "$TMP/bin" && cp "$ODIN" "$TMP/bin/odin$EXT" && chmod +x "$TMP/bin/odin$EXT"
    out=$(ODIN_RELEASES_API="http://127.0.0.1:$PORT/api/$1" "$TMP/bin/odin$EXT" upgrade $2 2>&1); code=$?
}
# python instead of cmp, windows' msys shell doesn't ship cmp
same_as() { "$PY" -c "import sys; sys.exit(open(sys.argv[1],'rb').read() != open(sys.argv[2],'rb').read())" "$TMP/bin/odin$EXT" "$1"; }

try old ""
[ $code = 0 ] && echo "$out" | grep -q "latest version" && same_as "$ODIN" && ok || bad "already up to date: $out"

try new --check
[ $code = 0 ] && echo "$out" | grep -q "update available" && same_as "$ODIN" && ok || bad "--check changed something: $out"

try badsum ""
[ $code != 0 ] && echo "$out" | grep -q "checksum" && same_as "$ODIN" && ok || bad "bad checksum not refused: $out"

try html ""
[ $code != 0 ] && echo "$out" | grep -q "doesn't look like" && same_as "$ODIN" && ok || bad "html page not refused: $out"

try nobin ""
[ $code != 0 ] && echo "$out" | grep -q "no odin-" && same_as "$ODIN" && ok || bad "missing download not caught: $out"

try missing ""
[ $code != 0 ] && echo "$out" | grep -q "GITHUB_TOKEN" && ok || bad "404 should mention GITHUB_TOKEN: $out"

try private ""
[ $code != 0 ] && same_as "$ODIN" && ok || bad "private repo without token should fail: $out"

GITHUB_TOKEN=letmein try private ""
[ $code = 0 ] && same_as "$TMP/served$EXT" && ok || bad "private repo with token didn't upgrade: $out"

try new ""
if [ $code = 0 ] && echo "$out" | grep -q "upgraded" && same_as "$TMP/served$EXT"; then ok; else bad "upgrade didn't install: $out"; fi
# the swapped-in binary has to actually run, and no temp files left lying around
"$TMP/bin/odin$EXT" version > /dev/null 2>&1 && ok || bad "upgraded binary won't run"
[ ! -e "$TMP/bin/odin$EXT.new" ] && ok || bad "left a .new file behind"

# can't write to where odin lives -> clear message, nothing broken (root ignores permissions, skip then)
if [ -z "$EXT" ] && [ "$(id -u)" != 0 ]; then
    cp "$ODIN" "$TMP/bin/odin" && chmod 555 "$TMP/bin"
    out=$(ODIN_RELEASES_API="http://127.0.0.1:$PORT/api/new" "$TMP/bin/odin" upgrade 2>&1); code=$?
    chmod 755 "$TMP/bin"
    [ $code != 0 ] && echo "$out" | grep -q "sudo" && same_as "$ODIN" && ok || bad "read-only install: $out"
fi

echo "$pass/$((pass + fail)) upgrade checks passed"
[ $fail = 0 ]
