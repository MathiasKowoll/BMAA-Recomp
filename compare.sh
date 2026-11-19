#!/bin/bash
# Run the interpreter and the translated code with the same invocation, each in its own empty jail, record their
# bridge calls, and say where the records first differ.
cd "$(dirname "$0")"
D="$(cd "$(dirname "$(readlink orig.bin)")/../../.." && pwd)"
T="${TMPDIR:-/tmp}"; LIMIT="${LIMIT:-600000}"   # AT=n also prints what runs between call n and the next (run-*.log)
export DR_SAME_RANDOM=1
[ -n "$AT" ] && export DR_TRACE_AT="$AT"
run() {  # name program seconds extra-args...
    local name=$1 prog=$2 secs=$3; shift 3
    local J; J="$(mktemp -d "$T/bmaa-jaula.XXXXXX")"
    FERAL_RUNTIME_DATA_FOLDER="$D" DR_TRACE_IMPORTS="$PWD/trace-$name.txt" DR_TRACE_LIMIT=$LIMIT "$prog" --entry --window 3 --init --gl \
        --limit 8000000000 "$@" --escritura "$J" "$(readlink orig.bin)" > "run-$name.log" 2>&1 &
    local pid=$! n=0
    while kill -0 $pid 2>/dev/null && [ $n -lt "$secs" ]; do /usr/bin/python3 -c "import time; time.sleep(1)"; n=$((n+1)); done
    kill -0 $pid 2>/dev/null && kill $pid
    rm -rf "$J"
}
# Under one name: the game asks for the name of its process, and a longer name is a different run.
mkdir -p build/as-interp build/as-aot
ln -f build/dr-interp build/as-interp/dr-game; ln -f build/dr-bmaa build/as-aot/dr-game
run interp ./build/as-interp/dr-game "${SECS:-60}" --skip-func 0x1c05da0 --skip-func 0x1b9b190
run aot ./build/as-aot/dr-game "${SECS:-60}"
/usr/bin/python3 - <<'PY'
import re
def load(path):
    # A handle the bridge gives the guest for a host object (0xE0000000 + n) is opaque to the guest, and which
    # number an object gets is the host's business (CoreFoundation hands back an object it already has when it
    # likes): every handle reads the same here.
    return [re.sub(r"\be0[0-9a-f]{6}\b", "<handle>", l) for l in open(path).read().splitlines()]
a = load("trace-interp.txt"); b = load("trace-aot.txt")
print("calls recorded: interpreter %d, translated %d" % (len(a), len(b)))
# The record shows four stack slots as arguments whatever the function takes, and what they point at. A slot past
# the real arguments holds whatever was left there, which is not always the same in two runs. So a difference
# there is counted, and what decides is the rest: which function, from where, the stack, the registers a callee
# keeps, and the result.
def key(l): return re.sub(r" a=\S+", "", re.sub(r" at=\S+", "", l))
loose = 0
for k, (x, y) in enumerate(zip(a, b)):
    if x == y: continue
    if key(x) == key(y): loose += 1; continue
    print("first difference at call %d (%d earlier calls differ only in the four slots shown as arguments)" % (k, loose))
    for l in a[max(0, k - 3):k]: print("   interp ", l)
    print("   interp ", x); print("   transl ", y)
    for j in range(k + 1, min(k + 3, len(a), len(b))): print("   interp ", a[j]); print("   transl ", b[j])
    break
else:
    print("no difference in the calls both made (%d differ only in the four slots shown as arguments)" % loose)
PY
