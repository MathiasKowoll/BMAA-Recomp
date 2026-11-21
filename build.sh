#!/bin/bash
# From an owner's copy of the game to the playable port, in one go:
#     ./build.sh "/path/to/Batman Arkham Asylum GOTY.app"
# Nothing of the game is shipped with these sources: the game's own executable is read where it is, translated to C
# (gen/) and compiled (build/), all on this Mac. It needs, side by side with this folder, Recomp-Framework and
# DecompRecomp; Apple's command line tools (clang, make), python3, and FFmpeg with pkg-config (brew install ffmpeg
# pkg-config) for the game's movies. Lines that start with "==" are the steps, for a program that shows progress.
# Run again after an update of the sources: only what changed is rebuilt.
set -euo pipefail
cd "$(dirname "$0")"
say() { echo "== $*"; }
die() { echo "!! $*" >&2; exit 1; }

APP="${1:-}"
[ -n "$APP" ] || { [ -L orig.bin ] && APP="$(cd "$(dirname "$(readlink orig.bin)")/../.." && pwd)"; }
[ -n "$APP" ] || die "usage: ./build.sh \"/path/to/Batman Arkham Asylum GOTY.app\""
EXE="$APP/Contents/MacOS/Batman Arkham Asylum GOTY"

say "1/5 checking this Mac and the game"
[ "$(uname -m)" = arm64 ] || die "this port is for Apple Silicon"
xcode-select -p >/dev/null 2>&1 || die "Apple's command line tools are missing: xcode-select --install"
command -v python3 >/dev/null || die "python3 is missing"
command -v pkg-config >/dev/null && pkg-config --exists libavformat libavcodec libavutil \
    || die "FFmpeg is missing (the game's movies play through it): brew install ffmpeg pkg-config"
[ -d ../Recomp-Framework/mac32 ] || die "Recomp-Framework is not beside this folder"
[ -d ../DecompRecomp/runtime ] || die "DecompRecomp is not beside this folder"
[ -f "$EXE" ] || die "not the game's application: $APP"
file -b "$EXE" | grep -q "i386" || die "this is not the 32-bit Intel executable of Feral's Mac version"
ln -sfn "$EXE" orig.bin

say "2/5 python tools (capstone, lief)"
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -c "import capstone, lief" 2>/dev/null || .venv/bin/pip install -q capstone lief

# The translation is redone when the game's executable or the translator changed.
STAMP="$(shasum -a 256 "$EXE" hle.txt ../Recomp-Framework/mac32/tools/recomp32.py | shasum -a 256 | cut -d' ' -f1)"
if [ ! -f gen/tables.c ] || [ "$(cat gen/.stamp 2>/dev/null)" != "$STAMP" ]; then
    say "3/5 translating the game's code to C (several minutes)"
    make gen
    echo "$STAMP" > gen/.stamp
else
    say "3/5 the translation is up to date"
fi

say "4/5 compiling (several minutes the first time)"
make -j"$(sysctl -n hw.ncpu)" dr

[ -x build/dr-bmaa ] || die "the build did not produce build/dr-bmaa"
say "5/5 done: ./play.sh starts the game (Steam must be running)"
