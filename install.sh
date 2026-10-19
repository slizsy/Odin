#!/bin/sh
# installs odin on mac or linux:
#   curl -fsSL https://raw.githubusercontent.com/slizsy/Odin/main/install.sh | sh
set -eu

REPO="slizsy/Odin"
BASE="https://github.com/$REPO/releases/latest/download"

say() { printf '%s\n' "$*"; }
die() { say "odin: $*" >&2; exit 1; }

case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) die "this installer is for mac and linux. on windows run install.ps1 instead" ;;
esac
case "$(uname -m)" in
    x86_64 | amd64) arch=x86_64 ;;
    arm64 | aarch64) arch=arm64 ;;
    *) die "no ready-made build for $(uname -m) yet, build it yourself with: make" ;;
esac
asset="odin-$os-$arch"
[ "$os" = macos ] && asset="odin-macos-universal"

command -v curl >/dev/null || die "need curl to download"
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

say "downloading $asset..."
curl -fsSL "$BASE/$asset" -o "$tmp" || die "download failed (is there a release yet?)"

# make sure we got exactly what was published
want=$(curl -fsSL "$BASE/SHA256SUMS" | awk -v a="$asset" '$2 == a { print $1 }')
if command -v sha256sum >/dev/null; then got=$(sha256sum "$tmp" | cut -d' ' -f1)
else got=$(shasum -a 256 "$tmp" | cut -d' ' -f1); fi
[ -n "$want" ] && [ "$want" = "$got" ] || die "checksum didn't match, download got mangled. try again"
chmod +x "$tmp"

# /usr/local/bin if we can, ~/.local/bin if we can't
dest=/usr/local/bin
if [ -w "$dest" ] || { [ ! -e "$dest" ] && [ -w "$(dirname "$dest")" ]; }; then
    mkdir -p "$dest" && mv "$tmp" "$dest/odin"
elif command -v sudo >/dev/null; then
    say "need your password to put odin in $dest"
    sudo mkdir -p "$dest" && sudo mv "$tmp" "$dest/odin"
else
    dest="$HOME/.local/bin"
    mkdir -p "$dest" && mv "$tmp" "$dest/odin"
    case ":$PATH:" in *":$dest:"*) ;; *) say "add this to your shell profile: export PATH=\"\$PATH:$dest\"" ;; esac
fi
trap - EXIT

say "installed to $dest/odin"
"$dest/odin" version
say "run 'odin' to get started"
