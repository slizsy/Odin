# Odin

```
 ▒█████  ▓█████▄  ██▓ ███▄    █         ᛟ  ᛞ  ᛁ  ᚾ
▒██▒  ██▒▒██▀ ██▌▓██▒ ██ ▀█   █      ▄▄▀▀▀▀▀▀▀▀▀▀▀▀▀▄▄
▒██░  ██▒░██   █▌▒██▒▓██  ▀█ ██▒   ▄▀    ▄▄█████▄▄    ▀▄
▒██   ██░░▓█▄   ▌░██░▓██▒  ▐▌██▒   █   ▄██▀▀   ▀▀██▄   █
░ ████▓▒░░▒████▓ ░██░▒██░   ▓██░   █  ██   ▄███▄   ██  █
░ ▒░▒░▒░  ▒▒▓  ▒ ░▓  ░ ▒░   ▒ ▒    █  ██   ▀███▀   ██  █
  ░ ▒ ▒░  ░ ▒  ▒  ▒ ░░ ░░   ░ ▒░   █   ▀██▄▄   ▄▄██▀   █
░ ░ ░ ▒   ░ ░  ░  ▒ ░   ░   ░ ░    ▀▄    ▀▀█████▀▀    ▄▀
    ░ ░     ░     ░           ░      ▀▀▄▄▄▄▄▄▄▄▄▄▄▄▄▀▀
          ░                           huginn · muninn
```

the all-seeing osint toolkit. made by slizsy.

one fast binary, no runtime, no dependencies. right now it does metadata extraction,
more modules (usernames, emails, domains, phones, ips) are on the way.

## install

needs go 1.22+

```sh
go install github.com/slizsy/Odin/cmd/odin@latest
```

or build it yourself:

```sh
git clone https://github.com/slizsy/Odin && cd Odin
go build -o odin ./cmd/odin
```

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

short flags stack, so `-rfq` works.

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

## use it right

only point this at stuff you're allowed to look at: your own files, ctfs, authorized
investigations. it's also great for checking your own files before you post them.

## dev

```sh
go test ./...
go test ./internal/meta -fuzz FuzzParse -fuzztime 30s   # throw junk at the parsers
```
