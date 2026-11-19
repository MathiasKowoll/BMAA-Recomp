#!/bin/bash
# Run the interpreter and the translated code with the same invocation, each in its own empty jail, record their
# bridge calls, and say where the records first differ.
cd "$(dirname "$0")"
D="$(cd "$(dirname "$(readlink orig.bin)")/../../.." && pwd)"
T="${TMPDIR:-/tmp}"; LIMIT="${LIMIT:-600000}"   # AT=n also prints what runs between call n and the next (run-*.log)
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
run interp ./build/dr-interp 60 --skip-func 0x1c05da0 --skip-func 0x1b9b190
run aot ./build/dr-bmaa 60
/usr/bin/python3 - <<'PY'
a = open("trace-interp.txt").read().splitlines(); b = open("trace-aot.txt").read().splitlines()
print("calls recorded: interpreter %d, translated %d" % (len(a), len(b)))
for k, (x, y) in enumerate(zip(a, b)):
    if x != y:
        print("first difference at call", k)
        for l in a[max(0, k - 3):k]: print("   both   ", l)
        print("   interp ", x); print("   transl ", y)
        for j in range(k + 1, min(k + 4, len(a), len(b))): print("   interp ", a[j]); print("   transl ", b[j])
        break
else:
    print("no difference in the calls both made")
PY
