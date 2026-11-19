#!/usr/bin/env python3
"""After `AT=n ./compare.sh`: inside the window between bridge call n and the next, compare the functions the
interpreter enters (taken from its instruction trace) with the ones the translated code enters, and show the
interpreter's instructions up to the first function only one of them enters."""
import re, sys
def cut(path):
    L = open(path, errors="replace").read().splitlines()
    a = [k for k, l in enumerate(L) if "[window] open" in l]; b = [k for k, l in enumerate(L) if "[window] closed" in l]
    if not a: sys.exit("no window in " + path)
    return L[a[0] + 1:(b[0] if b else len(L))]
I, A = cut("run-interp.log"), cut("run-aot.log")
ins = []
for l in I:
    m = re.match(r"\s+([0-9a-f]{8})\s+(.*)", l)
    if m: ins.append((int(m.group(1), 16), m.group(2)))
calls = [k for k in range(len(ins) - 1) if ins[k][1].startswith("call") and ins[k + 1][0] != ins[k][0] + 5 and ins[k + 1][0] < 0xfe000000]
ent = [ins[k + 1][0] for k in calls]
fnl = [l for l in A if "[fn]" in l]
fn = [int(re.match(r"\s+\[fn\] ([0-9a-f]{8})", l).group(1), 16) for l in fnl]
print("in the window: interpreter %d instructions and %d function entries, translated %d entries" % (len(ins), len(ent), len(fn)))
n = int(sys.argv[1]) if len(sys.argv) > 1 else 40
for k in range(max(len(ent), len(fn))):
    x = ent[k] if k < len(ent) else None; y = fn[k] if k < len(fn) else None
    if x == y: continue
    print("entry %d: interpreter %s, translated %s" % (k, "%08x" % x if x is not None else "none", "%08x" % y if y is not None else "none"))
    print("translated, its last entries:")
    for l in fnl[max(0, k - 3):k + 1]: print("   ", l.strip()[:200])
    idx = calls[k] if k < len(calls) else len(ins) - 1
    print("interpreter, the instructions before:")
    for a, t in ins[max(0, idx - n):idx + 2]: print("    %08x %s" % (a, t[:80]))
    break
else:
    print("the same entries")
