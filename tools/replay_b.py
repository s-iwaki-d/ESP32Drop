#!/usr/bin/env python3
"""Offline replay harness for the AWDL AW-grid-origin (B) estimator.

Why this exists: the firmware once latched a per-sender B that was 8 AW
(131ms, 2 chanseq slots) away from the truth, froze it permanently, and transmitted
into an empty window until a human noticed the badge had vanished from the AirDrop
UI. Nothing in the firmware could tell. This harness replays a recorded PSF trace
through a candidate estimator and scores it, so that failure is caught on a laptop.

Input: a SYNC_TRACE capture, lines of the form
  PS <src-mac-hex> rem=<n> aws=<n> rx=<u32> ptx=<u32> ttx=<u32>

Definitions (all arithmetic mod AWC; AWC = 2^20 and 2^32 is a multiple of it, so
uint32 differences are exact across counter wrap):
  k   = aws mod 64                          the frame's AW index within its cycle
  g   = (ptx - k*AW) mod AWC
  B   satisfies ((ptx - B) mod AWC) div AW == k  for every frame of that sender,
      which is equivalent to  B in (g - AW, g]  -- one interval of width AW per frame.
  phase = (rx - ptx + B) mod AWC            the mesh AWC boundary in OUR clock,
      i.e. exactly what handle_sync() computes and feeds the window PLL.
"""
import re, sys, statistics as st
from collections import Counter, defaultdict

TU  = 1024
AW  = 16 * TU          # 16384
AWC = 64 * AW          # 1048576 == 2^20
MASK = AWC - 1

def parse(path):
    rows = []
    pat = re.compile(r'^PS (\w{12}) rem=(-?\d+) aws=(\d+) rx=(\d+) ptx=(\d+) ttx=(\d+)')
    for line in open(path, errors="replace"):
        m = pat.match(line)
        if m:
            src, rem, aws, rx, ptx, ttx = m.groups()
            rows.append(dict(src=src, rem=int(rem), aws=int(aws),
                             rx=int(rx), ptx=int(ptx), ttx=int(ttx)))
    return rows

def g_of(r):
    return (r['ptx'] - (r['aws'] & 63) * AW) & MASK

def fits(B, rows):
    """How many frames satisfy the AW-index constraint for this B."""
    return sum(1 for r in rows if ((r['ptx'] - B) & MASK) // AW == (r['aws'] & 63))

def best_B(rows, coarse=256):
    """Reference B for a sender: maximises constraint satisfaction over ALL its frames."""
    best = (None, -1)
    for B in range(0, AWC, coarse):
        f = fits(B, rows)
        if f > best[1]: best = (B, f)
    B0 = best[0]
    for B in range(max(0, B0 - coarse), B0 + coarse + 1):
        f = fits(B, rows)
        if f > best[1]: best = (B & MASK, f)
    return best

def phase_of(r, B):
    return (r['rx'] - r['ptx'] + B) & MASK

def wrap(x, m=AWC):
    x %= m
    return x - m if x > m // 2 else x

def detrend(rows, B):
    """Fit phase(t) = p0 + slope*t (t in our-clock us). Returns p0, slope_ppm, resid_rms."""
    ph = [phase_of(r, B) for r in rows]
    t0 = rows[0]['rx']
    xs = [(r['rx'] - t0) & 0xffffffff for r in rows]
    # unwrap phase relative to the first sample
    ys, base = [], ph[0]
    for p in ph: ys.append(base + wrap(p - base))
    n = len(xs)
    mx, my = sum(xs)/n, sum(ys)/n
    den = sum((x-mx)**2 for x in xs)
    slope = sum((x-mx)*(y-my) for x, y in zip(xs, ys)) / den if den else 0.0
    resid = [y - (my + slope*(x-mx)) for x, y in zip(xs, ys)]
    rms = (sum(e*e for e in resid)/n) ** 0.5
    return (my - slope*mx) % AWC, slope*1e6, rms

# ============================================================================
# The estimator the firmware ships today, replicated exactly
# ============================================================================

def est_current(g_ring):
    """Exactly the original firmware's C estimate_B(). Takes the (idx+1)-th
    smallest of the folded ring to tolerate a couple of low outliers, and refuses if
    the surviving samples do not fit inside one AW."""
    n = len(g_ring)
    if n < 8: return None
    ref0 = g_ring[0]
    d = sorted(wrap((g - ref0) & MASK) for g in g_ring)
    idx = 2 if n >= 12 else 1
    lo = d[idx]
    if (d[n-1] - lo) >= AW: return None
    return (ref0 + lo) & MASK

# ============================================================================
# Characterisation + the reference gauge trajectory
# ============================================================================

def characterise(path=".build/trace3.txt", verbose=True):
    """Per-sender reference B, phase drift, and phase-residual rms. Returns
    (rows, by_src, ref, usable_srcs)."""
    rows = parse(path)
    by = defaultdict(list)
    for r in rows: by[r['src']].append(r)
    ref = {}
    for src, rs in sorted(by.items(), key=lambda kv: -len(kv[1])):
        B, f = best_B(rs)
        cnt = Counter(((r['aws'] - (r['ptx'] // AW)) & 63) for r in rs)
        ks = sorted(cnt)
        p0, ppm, rms = detrend(rs, B)
        ref[src] = dict(B=B, fit=f, n=len(rs), span_aw=max(ks)-min(ks),
                        p0=p0, ppm=ppm, rms=rms, t0=rs[0]['rx'])
        if verbose:
            print(f"  {src} n={len(rs):5d} B_ref={B:7d} fit={100*f/len(rs):5.1f}% "
                  f"span={max(ks)-min(ks):2d}AW drift={ppm:+8.1f}ppm resid_rms={rms:8.0f}us")
    # "Usable" = its own phase holds still with its own B. This is the property the
    # gauge depends on, and it separates the population by 4 orders of magnitude.
    usable = [s for s, d in ref.items() if d['rms'] < 5000 and abs(d['ppm']) < 200]
    if verbose: print(f"  usable senders: {usable}")
    return rows, by, ref, usable

def ref_gauge_fn(ref, usable):
    """The mesh AWC boundary in OUR clock as a function of our-clock time, taken as
    the consensus of the usable senders (measured to agree within ~419us)."""
    if not usable: raise RuntimeError("no usable sender: cannot build a reference gauge")
    def f(t):
        vals = []
        for s in usable:
            d = ref[s]
            vals.append((d['p0'] + d['ppm']/1e6 * ((t - d['t0']) & 0xffffffff)) % AWC)
        base = vals[0]
        return (base + sum(wrap(v - base) for v in vals)/len(vals)) % AWC
    return f

# ============================================================================
# Streaming scorer: what matters is whether the GAUGE ends up right
# ============================================================================
# A candidate sees the PSF frames in arrival order and, for each one, returns either
# None ("do not feed this to the window PLL") or an absolute phase sample
# (rx - ptx + B_sender) mod AWC. Scoring compares every fed sample against the
# reference gauge. A sample that is a whole AW or more out is the failure that put
# the badge 131ms off its window; a sample in the right 4-AW slot is fine.

def score_stream(make_candidate, path=".build/trace3.txt", label="", warmup_s=5.0,
                 rows=None, by=None, ref=None, usable=None, verbose=True):
    if rows is None: rows, by, ref, usable = characterise(path, verbose=False)
    gauge = ref_gauge_fn(ref, usable)
    cand = make_candidate()
    t_start = rows[0]['rx']
    fed = good_slot = wrong_aw = skipped = 0
    first_fed_t = None
    per_src = Counter()
    for r in rows:
        out = cand.on_frame(dict(r))
        if (r['rx'] - t_start) & 0xffffffff < warmup_s * 1e6:
            continue
        if out is None:
            skipped += 1
            continue
        fed += 1
        if first_fed_t is None: first_fed_t = ((r['rx'] - t_start) & 0xffffffff) / 1e6
        err = wrap(int(out) - int(gauge(r['rx'])))
        if abs(err) < AW:            good_slot += 1
        else:                        wrong_aw += 1; per_src[r['src']] += 1
    tot = fed + skipped
    if verbose:
        print(f"\n== gauge score: {label} ==")
        print(f"   frames after warmup : {tot}")
        print(f"   fed to PLL          : {fed} ({100*fed/tot:.1f}%)   skipped {skipped}")
        if fed:
            print(f"   within 1 AW of truth: {good_slot} ({100*good_slot/fed:.1f}% of fed)")
            print(f"   >= 1 AW WRONG       : {wrong_aw} ({100*wrong_aw/fed:.1f}% of fed)"
                  + ("   <<< this is the failure mode" if wrong_aw else "   <- clean"))
            if per_src: print(f"   wrong samples by sender: {dict(per_src)}")
        print(f"   first fed sample at : {first_fed_t}s" if first_fed_t else "   never fed anything")
    return dict(fed=fed, good=good_slot, wrong=wrong_aw, skipped=skipped,
                tot=tot, first=first_fed_t)

# ---- candidate: exactly what the firmware ships today ----------------------
class CandCurrent:
    """Replicates handle_sync(): per-sender 16-entry g ring, latch B the first time
    estimate_B() is confident, then feed (rx-ptx+B) forever. No validation."""
    WIN = 16
    def __init__(self):
        self.ring = defaultdict(list)
        self.B = {}
    def on_frame(self, r):
        s = r['src']
        if s not in self.B:
            ring = self.ring[s]
            ring.append(g_of(r))
            if len(ring) > self.WIN: ring.pop(0)
            if len(ring) == self.WIN:
                B = est_current(ring)
                if B is not None: self.B[s] = B
            return None
        return (r['rx'] - r['ptx'] + self.B[s]) & MASK

if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else ".build/trace3.txt"
    print(f"# {path}")
    rows, by, ref, usable = characterise(path)
    g = ref_gauge_fn(ref, usable)
    score_stream(CandCurrent, rows=rows, by=by, ref=ref, usable=usable,
                 label="CURRENT firmware (latch first confident B, no validation)")
