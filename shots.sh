#!/bin/bash
# Run the game as Steam would for $1 seconds (default 45) and, $2 seconds in (default 30), capture every window the
# process has into build/shots/. The game's first dialog is answered without being shown (DR_DIALOGO=1); Steam must be
# running. Extra environment (DR_LOG_OBJC=1 ...) passes through.
cd "$(dirname "$0")"
[ -x build/wins ] || swiftc -O tools/wins.swift -o build/wins
rm -rf build/shots; mkdir -p build/shots
DR_DIALOGO=${DR_DIALOGO:-1} SteamAppId=35140 SteamGameId=35140 ./dr-try.sh "${1:-45}" > build/shots/try.out 2>&1 &
/usr/bin/python3 -c "import time; time.sleep(${2:-30})"
# SHOT_USR2=1: ask the process for its view and page report (dbg.m) four seconds before the capture
if [ -n "$SHOT_USR2" ] && pgrep -x dr-bmaa >/dev/null; then kill -USR2 "$(pgrep -x dr-bmaa)"; /usr/bin/python3 -c "import time; time.sleep(4)"; fi
./build/wins 2>/dev/null > build/shots/wins.txt
i=0
while read -r id rest; do i=$((i+1)); screencapture -x -o -l "$id" "build/shots/w$i.png" 2>/dev/null; done < build/shots/wins.txt
cat build/shots/wins.txt
wait
cat build/shots/try.out
