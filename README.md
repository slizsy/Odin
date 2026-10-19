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
(basically every system already has them). right now it does metadata extraction and
username hunting, more modules (emails, domains, phones, ips) are on the way.

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
your user can't write (like `/usr/local/bin`), run `sudo odin upgrade`. while the repo is
private, set `GITHUB_TOKEN` to a token that can read it first.

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

## username hunting: `odin user`

```sh
odin user johndoe                    # check 45 sites, show where it exists
odin user johndoe jdoe99 -a          # several names, also list the misses
odin user johndoe --json | jq        # json for scripting
odin user --list                     # see every site it checks
odin user --selftest                 # make sure every site's rule still works
```

| flag | what it does |
| --- | --- |
| `-a`, `--all` | also list sites where the name wasn't found |
| `-t`, `--timeout <sec>` | per site timeout, default 15 |
| `--sites <file>` | use your own site list |
| `--list` | print the site list and exit |
| `--selftest` | test every site with a known real account + a gibberish name |

all requests go out in parallel, so the whole list takes a few seconds.

every result is one of three things:

- **found**: the account exists
- **not found**: the site said there's no such account
- **couldn't check**: the site blocked us (403, rate limit, bot check page, timeout).
  odin won't guess here, so you don't get fake hits

sites change their pages all the time, so run `odin user --selftest` now and then. it flags
any site whose rule went stale. instagram, x, tiktok and facebook aren't in the list on
purpose: they show a login wall for every name, so all you'd get is false positives.

want to go through tor or a proxy? libcurl picks it up from the environment:
`ALL_PROXY=socks5h://127.0.0.1:9050 odin user johndoe`

### adding sites

the list is plain text, one site per line:

```
# name   | url to check                     | check                 | profile url (optional) | known real user (optional)
GitHub   | https://github.com/{}            | status                |                        | torvalds
Steam    | https://steamcommunity.com/id/{} | absent:could not be found |                    | gabelogannewell
```

`{}` is where the username goes. the check can be:

- `status`: 200 = found, 404 = not found
- `absent:<text>`: found unless the page contains that text
- `present:<text>`: found only if the page contains that text

save your own list to a file and run `odin user --sites mysites.txt johndoe`, or add lines
to the built in list in `src/modules/user/sites.c` and run `make`.

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
    user/             the `odin user` command: site list, parallel checker, output
    upgrade/          the `odin upgrade` command
    usage.h           the help text
  core/               the plumbing: results, file reading, folder walking, sha256, string helpers
  parsers/
    parsers.c         sniffs what a file actually is and sends it to the right parser
    images/           jpeg, png, webp, exif, iptc
    docs/             pdf, office/odf, xmp, a tiny xml scanner
    archive/          zip reader (docx and friends are zips)
  analysis/           turns raw fields into findings
  output/             pretty terminal output + json
  external/           exiftool hookup
  ui/                 colors + the banner
tests/                unit tests, fuzz harness, fake websites for the user module
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
                       # odin upgrade vs a fake github
make STATIC=1          # self-contained binary, what the downloads are
make fuzz FUZZ_TIME=60 # throws random junk at the parsers (needs clang + libfuzzer)
make clean
```
