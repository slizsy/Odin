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

osint toolkit written in c. one small native binary, the only thing it needs is zlib
(which basically every system already has). right now it does metadata extraction,
more modules (usernames, emails, domains, phones, ips) are on the way.

## build

```sh
git clone https://github.com/slizsy/Odin && cd Odin
make
./odin
```

needs a c compiler (gcc or clang) and zlib headers:

- debian / ubuntu / kali: `sudo apt install build-essential zlib1g-dev`
- fedora: `sudo dnf install gcc make zlib-devel`
- arch: `sudo pacman -S base-devel zlib`
- macos: `xcode-select --install` (zlib comes with it)
- windows: use wsl

`sudo make install` drops it in `/usr/local/bin` so you can run `odin` from anywhere.

## usage

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

## what it reads

| format | what it pulls |
| --- | --- |
| jpeg / tiff / camera raw / webp | exif (camera, serial numbers, owner, software, timestamps), gps, xmp, iptc (photographer, city, country), comments |
| png | text chunks, exif, xmp, last modified time |
| pdf | info dict from every save (so edited fields show their old values), xmp, version, how many times it was re-saved |
| docx / xlsx / pptx | author, last modified by, company, app + version, edit time, template paths, file links to local paths or network shares, custom props, and exif/gps from every image inside the doc |
| odt / ods / odp | same deal from meta.xml |
| anything else | hunts for embedded xmp packets (psd, ai, mp4, svg...) |

## findings

odin doesn't just dump fields, it flags the stuff that matters:

- **high**: gps location (with a maps link), usernames leaking through file paths (`C:\Users\bob\...`, `/home/bob/...`)
- **medium**: names / authors, device serial numbers, emails, network share hostnames, city/country tags, fields that changed between pdf saves
- **info**: device make/model, software versions, edit time

## where stuff lives

```
src/
  main.c              command line handling
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
tests/                unit tests + fuzz harness
```

adding a new file format = new parser in `parsers/`, then one line in `parsers/parsers.c`.

## use it right

only point this at stuff you're allowed to look at: your own files, ctfs, authorized
investigations. it's also great for checking your own files before you post them.

## dev

```sh
make test              # unit tests, built with address sanitizer
make fuzz FUZZ_TIME=60 # throws random junk at the parsers (needs clang + libfuzzer)
make clean
```
