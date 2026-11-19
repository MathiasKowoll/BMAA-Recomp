#!/usr/bin/env python3
"""Run the built game with a time limit and say where it stopped: its exit, the imports it missed, its last lines."""
import collections, os, re, subprocess, sys, time
limit = int(sys.argv[1]) if len(sys.argv) > 1 else 60
res = os.path.join(os.path.dirname(os.path.realpath("orig.bin")), "..", "Resources")
t = time.time()
with open("run.log", "w") as log:
    try:
        r = subprocess.run(["build/BMAA", "orig.bin", res], stdout=log, stderr=log, timeout=limit, env=dict(os.environ, **dict(a.split("=", 1) for a in sys.argv[2:])))
        print("exit %d after %.1f s" % (r.returncode, time.time() - t))
    except subprocess.TimeoutExpired:
        print("still running after %d s: stopped" % limit)
lines = open("run.log", errors="replace").read().splitlines()
lax = collections.Counter(re.match(r"\[lax\] (\S+)", l).group(1) for l in lines if l.startswith("[lax]"))
print("%d lines; %d imports the runtime does not have:" % (len(lines), len(lax)))
print("  " + " ".join(sorted(lax)))
print("\n".join(l[:230] for l in [l for l in lines if not l.startswith("[lax]")][-14:]))
