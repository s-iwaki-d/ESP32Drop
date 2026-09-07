#!/usr/bin/env python3
"""Grade the host replay of the C gauge (tools/gauge_test.c) with the same reference
gauge the Python bake-off used, so the C is measured on exactly the same yardstick.

  cc -O2 -o /tmp/gauge_test tools/gauge_test.c && \
  /tmp/gauge_test .build/trace3.txt > /tmp/gauge_out.txt && \
  python3 tools/score_gauge.py /tmp/gauge_out.txt .build/trace3.txt
"""
import sys, re
from collections import Counter
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from replay_b import AW, AWC, MASK, wrap, characterise, ref_gauge_fn

out_path   = sys.argv[1] if len(sys.argv) > 1 else "/tmp/gauge_out.txt"
trace_path = sys.argv[2] if len(sys.argv) > 2 else ".build/trace3.txt"
WARMUP_S   = 5.0

rows, by, ref, usable = characterise(trace_path, verbose=False)
gauge = ref_gauge_fn(ref, usable)
t_start = rows[0]['rx']

fed = good = wrong = skipped = 0
first = None
per_src = Counter()
errs = []
for line in open(out_path):
    m = re.match(r'^OUT (\d+) (\w{12}) (NONE|\d+)$', line.strip())
    if not m: continue
    rx, src, val = int(m.group(1)), m.group(2), m.group(3)
    if ((rx - t_start) & 0xffffffff) < WARMUP_S * 1e6: continue
    if val == "NONE":
        skipped += 1; continue
    fed += 1
    if first is None: first = ((rx - t_start) & 0xffffffff) / 1e6
    e = wrap(int(val) - int(gauge(rx)))
    errs.append(abs(e))
    if abs(e) < AW: good += 1
    else: wrong += 1; per_src[src] += 1

tot = fed + skipped
errs.sort()
print(f"== C gauge (awdl_gauge.h) replayed on {trace_path} ==")
print(f"   frames after warmup : {tot}")
print(f"   fed to PLL          : {fed} ({100*fed/tot:.1f}%)   skipped {skipped}")
if fed:
    print(f"   within 1 AW of truth: {good} ({100*good/fed:.1f}% of fed)")
    print(f"   >= 1 AW WRONG       : {wrong} ({100*wrong/fed:.1f}% of fed)"
          + ("   <<< FAIL" if wrong else "   <- clean"))
    if per_src: print(f"   wrong by sender     : {dict(per_src)}")
    print(f"   |err| median={errs[len(errs)//2]}us p99={errs[int(len(errs)*.99)]}us "
          f"worst={errs[-1]}us ({errs[-1]/AW:.3f} AW -> {AW/max(errs[-1],1):.2f}x margin)")
print(f"   first fed sample at : {first}s" if first else "   never fed anything")
sys.exit(1 if wrong else 0)
