#!/bin/bash
# Run the translated game under DecompRecomp's runtime with its reference invocation, in an empty write jail,
# for at most $1 seconds (default 100). The log goes to dr-run.log.
cd "$(dirname "$0")"
D="$(cd "$(dirname "$(readlink orig.bin)")/../../.." && pwd)"
J="$(mktemp -d "${TMPDIR:-/tmp}/bmaa-jaula.XXXXXX")"
# SKIP_LAUNCHER=1: a copy of the game's preferences with GameOptionsDialogShouldShow off goes into the write jail,
# which the game reads before the real file (the real one is not touched). NOT ENOUGH YET: the launcher still shows,
# so that key is read from somewhere else (see notes). Kept as the place to finish it.
P="$HOME/Library/Application Support/Feral Interactive/Batman Arkham Asylum GOTY/Preferences Data"
if [ -n "$SKIP_LAUNCHER" ] && [ -f "$P" ]; then
    mkdir -p "$J$(dirname "$P")"
    /usr/bin/python3 - "$P" "$J$P" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
t = open(src).read()
t = re.sub(r'\s*<value name="GameOptionsDialogShouldShow"[^>]*>[^<]*</value>', '', t)
t = t.replace('<key name="Setup">', '<key name="Setup">\n                        <value name="GameOptionsDialogShouldShow" type="integer">0</value>', 1)
open(dst, 'w').write(t)
PY
fi
FERAL_RUNTIME_DATA_FOLDER="$D" ./build/dr-bmaa --entry --window "${WINDOW:-100000}" --init ${GLMODE:---gl-host} --limit 8000000000 \
    --escritura "$J" "$(readlink orig.bin)" > dr-run.log 2>&1 &
pid=$!; n=0
while kill -0 $pid 2>/dev/null && [ $n -lt "${1:-100}" ]; do /usr/bin/python3 -c "import time; time.sleep(1)"; n=$((n+1)); done
if kill -0 $pid 2>/dev/null; then echo "still running after ${n}s: stopped"; kill $pid; else wait $pid; echo "exit $? after about ${n}s"; fi
[ -n "$KEEP_JAIL" ] && echo "jail kept: $J" || rm -rf "$J"
