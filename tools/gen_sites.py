#!/usr/bin/env python3
"""builds odin's built-in username site list (src/modules/user/sites_data.c).

pulls the rules from maigret and sherlock (both MIT licensed) and keeps only sites that are:
  - popular (alexa rank <= --rank, or named in KEEP)
  - not disabled upstream
  - checkable by a normal https client: no tls fingerprinting, js challenges or login walls.
    those only ever come back "couldn't check" (that was most of the blocking)
plus data/sites-extra.json for anything they don't cover.

  git clone --depth 1 https://github.com/soxoj/maigret /tmp/maigret
  git clone --depth 1 https://github.com/sherlock-project/sherlock /tmp/sherlock
  python3 tools/gen_sites.py --maigret /tmp/maigret --sherlock /tmp/sherlock
"""
import argparse, json, os, re, sys

# sites that need a real browser or a login, no point asking
BAD_PROTECTION = {"tls_fingerprint", "cf_js_challenge", "aws_waf_js_challenge", "js_challenge",
                  "ddos_guard_challenge", "login", "custom_bot_protection", "cf_firewall"}
# worth keeping even without a good alexa rank
KEEP = {"HackerNews", "Docker Hub", "Gravatar", "osu!", "Codewars", "Chess", "Keybase"}
# sherlock-only sites worth having
SHERLOCK_ONLY = {"Codeberg", "Linktree"}
# nicer names than the upstream keys
RENAME = {"HackerNews": "Hacker News", "DEV Community": "DEV", "Chess": "Chess.com", "DeviantART": "DeviantArt",
          "NPM": "npm", "HuggingFace": "Hugging Face", "Itch.io": "itch.io"}


def subst(url, main):
    if url is None:
        return None
    url = url.replace("{urlMain}", (main or "").rstrip("/")).replace("{username}", "{}")
    return re.sub(r"(?<!:)/{2,}", "/", url)  # same slash cleanup maigret does


def from_maigret(name, v):
    s = {"name": RENAME.get(name, name), "url": subst(v["url"], v.get("urlMain"))}
    if v.get("urlProbe"):
        s["probe"] = subst(v["urlProbe"], v.get("urlMain"))
    if v["checkType"] == "message":
        s["check"] = "message"
        if v.get("presenseStrs"): s["present"] = v["presenseStrs"]
        if v.get("absenceStrs"): s["absent"] = v["absenceStrs"]
    else:
        s["check"] = "status"
    if v.get("errors"): s["blocked"] = v["errors"]
    if v.get("headers"): s["headers"] = {k: str(x) for k, x in v["headers"].items() if k.lower() != "connection"}
    if v.get("requestMethod", "GET").upper() == "POST":
        s["post"] = subst(json.dumps(v.get("requestPayload", {})), "")
    if v.get("regexCheck"): s["regex"] = v["regexCheck"]
    s["claimed"] = v.get("usernameClaimed")
    s["unclaimed"] = v.get("usernameUnclaimed")
    if v.get("protection"): s["protection"] = v["protection"]
    return s


def from_sherlock(name, v):
    s = {"name": RENAME.get(name, name), "url": v["url"]}
    if v.get("urlProbe"):
        s["probe"] = v["urlProbe"]
    if v["errorType"] == "message":
        s["check"] = "message"
        msgs = v["errorMsg"] if isinstance(v["errorMsg"], list) else [v["errorMsg"]]
        s["absent"] = msgs
    else:
        s["check"] = "status"
    if v.get("headers"): s["headers"] = v["headers"]
    if v.get("regexCheck"): s["regex"] = v["regexCheck"]
    s["claimed"] = v.get("username_claimed")
    return s


def usable_maigret(v):
    # "type" means the site takes some numeric id (steam_id, gaia_id...), not a username
    # "activation" means maigret fetches a fresh token first (twitter's guest token), we can't
    return (not v.get("disabled") and "url" in v and "engine" not in v and "urlSubpath" not in v and "type" not in v
            and "activation" not in v
            and v.get("checkType") in ("status_code", "message") and not v.get("requestHeadOnly")
            and not (set(v.get("protection", [])) & BAD_PROTECTION))


def c_pieces(js):
    # pure ascii json (non-ascii is \\u escaped), one short c string per array element so no
    # single literal gets anywhere near compiler limits (c99 only promises 4095 chars)
    out, line = [], ""
    for ch in js:
        line += "\\\\" if ch == "\\" else '\\"' if ch == '"' else ch
        if len(line) > 100 and ch in ",}]":
            out.append(line)
            line = ""
    if line:
        out.append(line)
    return "".join(f'    "{l}",\n' for l in out) + "    NULL,\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maigret", required=True, help="path to a maigret checkout")
    ap.add_argument("--sherlock", required=True, help="path to a sherlock checkout")
    ap.add_argument("--extra", default="data/sites-extra.json")
    ap.add_argument("--out", default="src/modules/user/sites_data.c")
    ap.add_argument("--rank", type=int, default=20000, help="keep sites with alexa rank up to this")
    a = ap.parse_args()

    mg = json.load(open(os.path.join(a.maigret, "maigret/resources/data.json")))["sites"]
    sh = json.load(open(os.path.join(a.sherlock, "sherlock_project/resources/data.json")))

    sites, seen = [], set()
    def key(url):  # same profile page = same site, whatever each project calls it
        return re.sub(r"^https?://(www\.)?", "", url.lower()).rstrip("/")
    def add(s):
        k = key(s["url"])
        if s["name"].lower() not in seen and k not in seen:
            seen.update({s["name"].lower(), k})
            sites.append({k: v for k, v in s.items() if v not in (None, [], {}, "")})

    picked = [(k, v) for k, v in mg.items() if usable_maigret(v)
              and ((v.get("alexaRank") or 10**9) <= a.rank or k in KEEP)]
    picked.sort(key=lambda kv: (kv[1].get("alexaRank") or 10**9, kv[0].lower()))
    for k, v in picked:
        add(from_maigret(k, v))
    for k in sorted(SHERLOCK_ONLY):
        if k in sh and sh[k].get("errorType") in ("status_code", "message"):
            add(from_sherlock(k, sh[k]))
    for s in json.load(open(a.extra))["sites"]:
        add(s)

    credit = ("site rules adapted from maigret (MIT, (c) 2020-2026 Soxoj) and sherlock "
              "(MIT, (c) 2019 Sherlock Project), see THIRD_PARTY_NOTICES.md")
    body = json.dumps({"sites": sites}, ensure_ascii=True, separators=(",", ":"))
    with open(a.out, "w") as f:
        f.write("// generated by tools/gen_sites.py, don't edit by hand. rerun the script instead\n")
        f.write(f"// {len(sites)} sites. {credit}\n")
        f.write('#include "modules/user/sites.h"\n\n')
        f.write(f'const char *const sites_builtin_credit = "{credit}";\n\n')
        f.write("const char *const sites_builtin_json[] = {\n")
        f.write(c_pieces(body) + "};\n")
    print(f"wrote {a.out}: {len(sites)} sites ({len(picked)} from maigret)", file=sys.stderr)


if __name__ == "__main__":
    main()
