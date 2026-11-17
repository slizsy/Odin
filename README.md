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

privacy desktop app written in rust. drop files in, see everything they leak about you,
get clean copies back.

**everything runs on your machine.** odin never uploads anything or talks to the internet on
its own. the only things that ever open an outside site are links you click yourself, and
they say so when you hover them.

## what it does

### see what's in a file before you clean it
pick a file and odin lists every metadata field it found, sorted into groups:
**location, author, device, dates, software, ai / content credentials, other**.

- **red** = sensitive, can point at a person, a place or a specific device (gps, owner name,
  serial numbers, comment authors, usernames hiding in file paths, emails)
- **amber** = worth a look (dates, camera model, edit trails, hidden preview images)
- the "what stands out" box up top sums up the worst stuff in plain words

### strip ai tags + content credentials (c2pa)
odin finds and removes:
- c2pa / content credentials manifests (jpeg app11, png `caBX`, webp, heic / mp4 uuid boxes)
- the iptc "made by an ai" tag (`DigitalSourceType = trainedAlgorithmicMedia`)
- generator prompts + settings in png text chunks (stable diffusion, comfyui, invokeai, ...)
- "made with firefly / midjourney / dall-e / ..." in software and credit fields

heads up: this only covers **metadata**. invisible watermarks baked into the pixels
(like google's synthid) can't be seen or removed this way.

### batch mode
drag in a whole folder, a bunch of files, or both. odin walks subfolders (skips hidden files
and symlinks), scans everything in the background, then **clean all** does them in one go.
the list shows how bad each file is at a glance.

### filename scrubber
names like `IMG_20261116_104233.jpg` give away when you took it. odin flags names with
dates / app names in them, and the **file names** option at the bottom can rename the
cleaned copies:
- **keep** the original name
- **random**: `a3f91c0b72de.jpg`
- **generic**: `image-001.jpg`, `video-001.mp4`, `document-001.pdf`

### docs and pdfs
**word, excel, powerpoint** (and opendocument):
- author, last modified by, company, manager, template path, editing time, app version
- comments + comment authors (turn off **remove comments** to keep them)
- tracked changes: deleted text is really deleted, inserted text becomes normal text,
  the author names on every change go away
- revision ids (`rsid`) that link documents edited in the same session
- the saved-from folder excel keeps (`C:\Users\you\...`)
- the first-page thumbnail
- photos pasted into the doc get cleaned too (their gps included)

**pdf**:
- the info dictionary (author, creator, producer, dates) + the xmp packet
- comment / markup annotations (links and form fields stay)
- old versions from earlier saves (pdfs append on every save, odin rebuilds the file so only
  the current version is left)
- the file id

### video + audio
container metadata, per-stream tags, chapters, gps (iphone + android videos both store it),
device make / model, data tracks. done with ffmpeg in stream-copy mode, so the
video and audio aren't re-encoded: same quality, and it's quick.

works on mp4, mov, m4a, mkv, webm, avi, mp3, flac, wav, ogg.

### what originals get

nothing. odin never edits the file you gave it. cleaned copies go to an `odin-clean` folder
next to each file (or one folder you pick), and they're written to a temp file first so a
crash can't leave half a file behind. after cleaning, odin scans the copy again so you can
flip between **original** and **cleaned copy** and see what's left.

| format | what gets cleaned |
| --- | --- |
| jpeg | exif, xmp (+ extended), iptc / photoshop, c2pa, comments, embedded thumbnails, data after the image (motion photo clips etc) |
| png | text chunks, xmp, exif, time stamp, c2pa, private chunks, data after the image |
| webp, gif | exif, xmp, c2pa, comments, app blocks |
| heic / avif | exif + xmp blanked in place, uuid boxes (c2pa) |
| docx, xlsx, pptx, odt/ods/odp | see "docs and pdfs" |
| pdf | see "docs and pdfs" |
| video + audio | everything but the streams themselves (needs ffmpeg) |

by default odin keeps one tag, **orientation**, so phone photos don't end up sideways.
**keep colour profile** leaves the icc profile in so photos don't look washed out
(off by default, the profile name can be a fingerprint).

## install

grab the build for your os from the [releases page](https://github.com/slizsy/Odin/releases)
and run it. no installer.

### ffmpeg (for video + audio only)
odin doesn't ship ffmpeg. if it can't find one, the top bar says so and gives you a
**get ffmpeg** link (opens ffmpeg.org, only when you click it). odin looks in this order:
1. the `ODIN_FFMPEG` env var
2. your `PATH`
3. next to the odin exe

easy installs: `brew install ffmpeg` (mac), `winget install ffmpeg` (windows),
`sudo apt install ffmpeg` (debian / ubuntu).

## build it yourself

needs [rust](https://rustup.rs) 1.85+.

```sh
cargo run --release
```

on linux you also need the usual window libs:
`sudo apt install libxkbcommon-dev libgl1-mesa-dev libgtk-3-dev`

```sh
cargo test                 # engine tests (builds leaky test files, cleans them, checks)
cargo run -p odin-core --example dump -- photo.jpg          # print what the engine sees
cargo run -p odin-core --example dump -- --clean out/ a.jpg # clean + show what's left
```

### layout
```
core/   odin-core, the engine. reads + strips metadata, no ui, no network
  src/formats/   one file per format (jpeg, png, pdf, office, mp4 boxes, ...)
  tests/         round-trip tests
app/    the desktop app (egui)
```

## libraries

all well-maintained, all run locally:

| crate | used for |
| --- | --- |
| [eframe / egui](https://github.com/emilk/egui) | the window + ui |
| [rfd](https://github.com/PolyMeilex/rfd) | native open-file / folder dialogs |
| [kamadak-exif](https://github.com/kamadak/exif-rs) | decoding exif tags for the viewer |
| [lopdf](https://github.com/J-F-Liu/lopdf) | reading + rewriting pdfs |
| [flate2](https://github.com/rust-lang/flate2-rs) | zip / png compression |
| [getrandom](https://github.com/rust-random/getrandom) | random filenames |
| [image](https://github.com/image-rs/image) | saving screenshots in dev mode, tests |

jpeg, png, webp, gif, heic, mp4, zip and xml handling is written in odin itself so it
can rewrite files byte-for-byte without touching the pixels.

## the old c version

odin used to be a c terminal osint tool (username hunting, domain / ip / email / phone
lookups). that's still on the `main` branch history. this rewrite is just the privacy side.
