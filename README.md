# Odin

```
 ▒█████  ▓█████▄  ██▓ ███▄    █       ▄▄▀▀▀▀▀▀▀▀▀▀▀▀▀▄▄
▒██▒  ██▒▒██▀ ██▌▓██▒ ██ ▀█   █     ▄▀    ▄▄█████▄▄    ▀▄
▒██░  ██▒░██   █▌▒██▒▓██  ▀█ ██▒    █   ▄██▀▀   ▀▀██▄   █
▒██   ██░░▓█▄   ▌░██░▓██▒  ▐▌██▒    █  ██   ▄███▄   ██  █
░ ████▓▒░░▒████▓ ░██░▒██░   ▓██░    █  ██   ▀███▀   ██  █
░ ▒░▒░▒░  ▒▒▓  ▒ ░▓  ░ ▒░   ▒ ▒     █   ▀██▄▄   ▄▄██▀   █
  ░ ▒ ▒░  ░ ▒  ▒  ▒ ░░ ░░   ░ ▒░    ▀▄    ▀▀█████▀▀    ▄▀
░ ░ ░ ▒   ░ ░  ░  ▒ ░   ░   ░ ░       ▀▀▄▄▄▄▄▄▄▄▄▄▄▄▄▀▀
    ░ ░     ░     ░           ░          ᛟ  ᛞ  ᛁ  ᚾ
          ░

                      made by slizsy
```

osint toolkit written in c. one small native binary that only needs zlib and libcurl
(basically every system already has them). it does metadata extraction, username hunting,
and recon on domains, ips, emails and phone numbers.

## install

no compiling, no extra stuff to install. pick your os:

**mac / linux**, paste into a terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/slizsy/Odin/main/install.sh | sh
```

**windows**, paste into powershell:

```powershell
irm https://raw.githubusercontent.com/slizsy/Odin/main/install.ps1 | iex
```

then open a terminal and run `odin`. both installers check the download's sha256 before
installing anything.

**updating:** `odin upgrade` grabs the newest release for your os, checks its sha256, and swaps
itself in. `odin upgrade --check` just tells you if there's a new one. if odin lives somewhere
your user can't write (like `/usr/local/bin`), run `sudo odin upgrade`.

or grab the file yourself from the [releases page](https://github.com/slizsy/Odin/releases/latest):

| os | file |
| --- | --- |
| windows 10/11 (64-bit) | `odin-windows-x86_64.exe` |
| mac (apple silicon + intel) | `odin-macos-universal` |
| linux x86_64 (any distro) | `odin-linux-x86_64` |
| linux arm64 (pi, arm servers) | `odin-linux-arm64` |

on mac/linux run `chmod +x` on it first. if mac complains it "can't verify the developer"
(that happens when you download through a browser), run `xattr -d com.apple.quarantine odin-macos-universal`
once. the install script avoids this entirely.

windows smartscreen might warn about an unknown app the first time since it isn't code
signed yet, hit "more info" then "run anyway".

## build it yourself

```sh
git clone https://github.com/slizsy/Odin && cd Odin
make
./odin
```

needs a c compiler (gcc or clang) plus zlib and libcurl headers:

- debian / ubuntu / kali: `sudo apt install build-essential zlib1g-dev libcurl4-openssl-dev`
- fedora: `sudo dnf install gcc make zlib-devel libcurl-devel`
- arch: `sudo pacman -S base-devel zlib curl`
- macos: `xcode-select --install` (zlib and curl come with it)
- windows: [msys2](https://www.msys2.org) ucrt64 shell, then
  `pacman -S make mingw-w64-ucrt-x86_64-{gcc,pkgconf,curl,zlib}` and `make STATIC=1`

`sudo make install` drops it in `/usr/local/bin` so you can run `odin` from anywhere.

## metadata: `odin meta`

```sh
odin meta photo.jpg                  # everything in one file
odin meta -r ~/Downloads -f          # whole folder, findings only
odin meta report.pdf --json | jq     # json for scripting
odin meta -x weird.heic              # full dump via exiftool (if installed)
```

| flag | what it does |
| --- | --- |
| `-r`, `--recursive` | walk folders all the way down |
| `-f`, `--findings` | only show findings, skip the raw field dump |
| `-j`, `--json` | json output |
| `-x`, `--exiftool` | use exiftool for the field dump, reads basically every format |
| `-q`, `--quiet` | no banner |
| `--no-color` | plain text (also respects `NO_COLOR`) |

short flags stack, so `-rfq` works. use `--` before a filename that starts with a dash.

## interactive mode

just type `odin`. the banner shows once and you get a prompt where you run commands
without typing `odin` every time:

```
odin> meta ~/Downloads/IMG_1930.HEIC
odin> user johndoe -a
odin> help
odin> exit
```

- up / down arrows go back through what you typed, ctrl+a / ctrl+e / ctrl+w etc work like a normal terminal
- drag a file into the window to paste its path (spaces and all)
- ctrl+c stops a running scan and drops you back at the prompt, ctrl+d or `exit` leaves
- `odin meta ...` and `odin user ...` from your normal terminal still work exactly like before

## username hunting: `odin user`

```sh
odin user johndoe                    # check ~240 sites, show where it exists
odin user johndoe jdoe99 -a          # several names, also list the misses
odin user johndoe --json | jq        # json for scripting
odin user --list                     # see every site it checks
odin user --selftest                 # make sure every site's rule still works
```

| flag | what it does |
| --- | --- |
| `-a`, `--all` | also list sites where the name wasn't found |
| `-t`, `--timeout <sec>` | per site timeout, default 15 |
| `--sites <file>` | use your own site list (see below) |
| `--list` | print the site list and exit |
| `--selftest` | test every site with a known real account + a known fake one |

every result is one of three things:

- **found**: the account exists
- **not found**: the site said there's no such account (or its username rules say the name can't exist there)
- **couldn't check**: the site blocked us, rate limited us, showed a bot check page or timed out.
  odin won't guess here, so you don't get fake hits

### getting blocked less

- the built in list comes from [maigret](https://github.com/soxoj/maigret) and
  [sherlock](https://github.com/sherlock-project/sherlock)'s maintained site data, minus every site
  that needs a real browser (tls fingerprinting, javascript challenges, login walls). those can
  only ever say "couldn't check", so odin doesn't bother them
- it uses a site's hidden api address when there is one, those block a lot less than pages
- every request looks like one of 6 real browsers (chrome, firefox, safari, edge on windows / mac),
  with the headers that browser actually sends. cookies are kept across redirects
- max 2 requests per site at once. a dropped connection, 429 or 5xx gets one retry after a
  short pause as a different browser, and odin honors the site's `Retry-After`
- the site's own username rules are checked first, so it never asks a site about a name that
  can't exist there
- some sites block vpn and cloud ips no matter what. odin says so in the "couldn't check" reason

sites change their pages all the time, so run `odin user --selftest` now and then, it flags any
rule that went stale. want to go through tor or a proxy? libcurl picks it up from the environment:
`ALL_PROXY=socks5h://127.0.0.1:9050 odin user johndoe`

### your own site lists

`--sites` takes three kinds of file:

**whatsmyname's list** (700 sites, kept up to date by that project): download `wmn-data.json` from
https://github.com/WebBreacher/WhatsMyName and run `odin user --sites wmn-data.json johndoe`.

**plain text**, one site per line:

```
# name   | url to check                     | check                     | profile url (optional) | known real user (optional)
GitHub   | https://github.com/{}            | status                    |                        | torvalds
Steam    | https://steamcommunity.com/id/{} | absent:could not be found |                        | gabelogannewell
```

`{}` is where the username goes. the check is `status` (200 = found, 404 = not found),
`absent:<text>` (found unless the page has that text) or `present:<text>` (found only if it does).

**odin json**, same format as the built in list, for when you need more:

```json
{"sites": [{
  "name": "Example", "url": "https://example.com/u/{}", "probe": "https://api.example.com/users/{}",
  "check": "strict", "found_code": 200, "present": ["\"id\":"], "missing_code": 404, "absent": ["not found"],
  "headers": {"Accept": "application/json"}, "regex": "^[a-z0-9_]{3,20}$",
  "claimed": "realuser", "unclaimed": "noonewouldeverusethis7"
}]}
```

`check` is `status`, `message` (uses `present` / `absent`) or `strict` (whatsmyname style: found
only on `found_code` + `present`, not found only on `missing_code` + `absent`, anything else is
"couldn't check"). optional: `post` (send this body as a POST), `blocked` (`{"page text": "reason"}`),
`strip` (characters to drop from the username).

the built in list is generated, to refresh it from the latest maigret / sherlock data:

```sh
git clone --depth 1 https://github.com/soxoj/maigret /tmp/maigret
git clone --depth 1 https://github.com/sherlock-project/sherlock /tmp/sherlock
python3 tools/gen_sites.py --maigret /tmp/maigret --sherlock /tmp/sherlock
make
```

odin's own extra rules live in `data/sites-extra.json`. credits and licenses are in `THIRD_PARTY_NOTICES.md`.

## domain recon: `odin domain`

```sh
odin domain example.com              # dns, registration, subdomains
odin domain example.com --no-subs    # skip the (slower) crt.sh lookup
odin domain example.com --json | jq
```

pulls together everything public about a domain:

- **dns**: A, AAAA, MX, NS, CAA records, plus the SPF and DMARC records picked out of TXT
- **registration** (via [rdap](https://about.rdap.org), the modern whois): registrar, created /
  expires / updated dates, status flags, DNSSEC, nameservers
- **subdomains**: every hostname that's shown up in a tls certificate, from
  [crt.sh](https://crt.sh)'s certificate transparency logs. this is usually where the
  interesting stuff hides (`dev.`, `staging.`, `vpn.`, `internal.`...)

then it flags the stuff worth knowing: missing SPF/DMARC (spoofable mail), `p=none` dmarc
(not actually enforced), no DNSSEC, and how many subdomains are exposed.

## ip recon: `odin ip`

```sh
odin ip 8.8.8.8
odin ip 2606:4700:4700::1111          # v6 works too
odin ip 8.8.8.8 --json | jq
```

- **network** (rdap): who the block is allocated to, the CIDR, range, and the abuse contact
- **geo + asn**: country, region, city, the ASN and the ISP/org behind it
- **reverse dns**: the PTR record, if there is one

private / loopback / link-local addresses are called out instead of looked up (no point
asking the internet about `192.168.x.x`).

## email recon: `odin email`

```sh
odin email someone@example.com
odin email someone@example.com --json
```

no mail is ever sent. odin checks:

- **syntax**: is it even a valid address
- **deliverable**: does the domain actually have MX records (or at least resolve)
- **disposable**: is it one of ~10k known throwaway/temp-mail domains
- **role account**: `admin@`, `support@`, `info@`... (a team, not a person)
- **gravatar**: is there a public profile tied to the address (those often link other accounts)

## phone lookup: `odin phone`

```sh
odin phone +14155552671
odin phone "(020) 7183 8750" --country GB   # national format, pass the country
odin phone +14155552671 --json
```

fully offline, built from [libphonenumber](https://github.com/google/libphonenumber)'s
metadata: it works out the country, the line type (mobile / fixed / toll-free / voip...),
whether the number is actually valid, and normalizes it to E.164. use `-c`/`--country` for
numbers typed without a `+`.

the domain / ip / email lookups all use public records only (dns-over-https, rdap,
certificate transparency, ipwho.is, gravatar) and work through a proxy like everything else.
to refresh the bundled phone and disposable-email data:

```sh
git clone --depth 1 https://github.com/google/libphonenumber /tmp/lpn
python3 tools/gen_phone.py --xml /tmp/lpn/resources/PhoneNumberMetadata.xml
python3 tools/gen_disposable.py      # fetches the latest list
make
```

## what `odin meta` reads

| format | what it pulls |
| --- | --- |
| jpeg / heic (iphone) / avif / tiff / camera raw / webp | exif (camera, serial numbers, owner, software, timestamps), gps, xmp, iptc (photographer, city, country), comments |
| png | text chunks, exif, xmp, last modified time |
| pdf | info dict from every save (so edited fields show their old values), xmp, version, how many times it was re-saved |
| docx / xlsx / pptx | author, last modified by, company, app + version, edit time, template paths, file links to local paths or network shares, custom props, and exif/gps from every image inside the doc |
| odt / ods / odp | same deal from meta.xml |
| anything else | hunts for embedded xmp packets (psd, ai, mp4, svg...) |

## metadata findings

odin doesn't just dump fields, it flags the stuff that matters:

- **high**: gps location (with a maps link), usernames leaking through file paths (`C:\Users\bob\...`, `/home/bob/...`)
- **medium**: names / authors, device serial numbers, emails, network share hostnames, city/country tags, fields that changed between pdf saves
- **info**: device make/model, software versions, edit time

## where stuff lives

```
src/
  main.c              sends each command to its module
  modules/
    meta/             the `odin meta` command
    shell/            interactive mode (odin with no command)
    user/             the `odin user` command: site loaders, parallel checker, output
    domain/           the `odin domain` command: dns, rdap, crt.sh
    ip/               the `odin ip` command: rdap, geo, reverse dns
    email/            the `odin email` command + the bundled disposable-domain list
    phone/            the `odin phone` command + generated libphonenumber data
    upgrade/          the `odin upgrade` command
    usage.h           the help text
  core/               the plumbing: results, files, folders, sha256, json, https,
                      dns-over-https, regex, iso country names, os differences
  parsers/
    parsers.c         sniffs what a file actually is and sends it to the right parser
    images/           jpeg, png, webp, exif, iptc
    docs/             pdf, office/odf, xmp, a tiny xml scanner
    archive/          zip reader (docx and friends are zips)
  analysis/           turns raw fields into findings
  output/             pretty terminal output + json
  external/           exiftool hookup
  ui/                 colors, the banner, the line editor for interactive mode
tools/                gen_sites.py (usernames), gen_phone.py (phone data),
                      gen_disposable.py (temp-mail domains), gen_iso.py (country names)
data/                 odin's own extra username site rules
tests/                unit tests, fuzz harness, fake websites + fake dns/rdap servers
```

adding a new file format = new parser in `parsers/`, then one line in `parsers/parsers.c`.

## use it right

only point this at stuff you're allowed to look at: your own files, ctfs, authorized
investigations. it's also great for checking your own files before you post them.

## releases

every push builds and tests odin on linux, mac and windows (see `.github/workflows/build.yml`).
to publish a release, bump `ODIN_VERSION` in `src/ui/banner.h`, merge, then either:

- actions tab -> build -> run workflow -> type the version (like `v0.1.2`), or
- `git tag v0.1.2 && git push origin v0.1.2`

the release job refuses if the tag and `ODIN_VERSION` don't match.

## dev

```sh
make test              # unit tests (address sanitizer), username engine vs fake sites,
                       # domain/ip/email vs a fake dns+rdap server, odin upgrade vs fake github
make STATIC=1          # self-contained binary, what the downloads are
make fuzz FUZZ_TIME=60 # throws random junk at the parsers (needs clang + libfuzzer)
make clean
```
