#!/bin/bash
# Steam's own PLAY button starts the port:
#     ./steam-install.sh            puts a small launcher where the game's program is
#     ./steam-install.sh remove     puts the game's program back
# The game's program (32-bit Intel, which macOS no longer runs) is kept beside it, untouched, with ".orig" added to
# its name; the port reads it from there. The launcher is a script that starts this folder's steam-play.sh. Nothing
# in Steam's settings is changed. Steam's "Verify integrity of game files" puts the original back; run this again.
set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"
die() { echo "!! $*" >&2; exit 1; }
[ -L orig.bin ] || die "build the port first: ./build.sh \"/path/to/Batman Arkham Asylum GOTY.app\""
EXE="$(readlink orig.bin)"; EXE="${EXE%.orig}"
MARK="BMAA-Recomp launcher"

if [ "${1:-}" = remove ]; then
    [ -f "$EXE.orig" ] || { echo "== nothing to remove: the game's program is in place"; exit 0; }
    grep -q "$MARK" "$EXE" 2>/dev/null && rm -f "$EXE"
    [ -e "$EXE" ] && die "$EXE is not this launcher; left alone (the original is at $EXE.orig)"
    mv "$EXE.orig" "$EXE"
    ln -sfn "$EXE" orig.bin
    echo "== the game's own program is back in place"
    exit 0
fi

if [ ! -f "$EXE.orig" ]; then
    file -b "$EXE" | grep -q "i386" || die "$EXE is not the game's 32-bit program, and there is no .orig beside it"
    mv "$EXE" "$EXE.orig"
elif [ -f "$EXE" ] && ! grep -q "$MARK" "$EXE" 2>/dev/null; then
    # Steam put the game's program back (an update, a verify): the copy kept earlier is the same program
    file -b "$EXE" | grep -q "i386" && mv -f "$EXE" "$EXE.orig"
fi
cat > "$EXE" <<LAUNCH
#!/bin/bash
# $MARK: the game's own program is beside this file, with .orig added to its name.
exec "$HERE/steam-play.sh" "\$@"
LAUNCH
chmod +x "$EXE"
ln -sfn "$EXE.orig" orig.bin
echo "== Steam's PLAY now starts the port (the game's own program is kept as $(basename "$EXE").orig)"
