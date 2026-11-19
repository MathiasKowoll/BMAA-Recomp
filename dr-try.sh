#!/bin/bash
# Run the translated game under DecompRecomp's runtime with its reference invocation, in an empty write jail,
# for at most $1 seconds (default 100). The log goes to dr-run.log.
cd "$(dirname "$0")"
D="$(cd "$(dirname "$(readlink orig.bin)")/../../.." && pwd)"
J="$(mktemp -d "${TMPDIR:-/tmp}/bmaa-jaula.XXXXXX")"
FERAL_RUNTIME_DATA_FOLDER="$D" ./build/dr-bmaa --entry --window "${WINDOW:-20}" --init --gl --limit 8000000000 \
    --escritura "$J" "$(readlink orig.bin)" > dr-run.log 2>&1 &
pid=$!; n=0
while kill -0 $pid 2>/dev/null && [ $n -lt "${1:-100}" ]; do /usr/bin/python3 -c "import time; time.sleep(1)"; n=$((n+1)); done
if kill -0 $pid 2>/dev/null; then echo "still running after ${n}s: stopped"; kill $pid; else wait $pid; echo "exit $? after about ${n}s"; fi
rm -rf "$J"
