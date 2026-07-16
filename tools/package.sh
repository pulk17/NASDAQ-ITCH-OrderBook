#!/usr/bin/env bash
# Check that report/ is a complete, publishable static site and print exactly what
# would ship. Read-only unless you pass a feature budget.
#
#   tools/package.sh              # audit only
#   tools/package.sh 250          # …first prune every feature set to its top 250 symbols
#
# Cloudflare Pages limits: 25 MiB per file, 20,000 files per deployment. Both are
# checked below. The tape slices are already chunked under 20 MB by slice_itch.py;
# the feature files are per-symbol and lazily fetched, so the only real cost of
# keeping more of them is repo size.
set -euo pipefail
cd "$(dirname "$0")/.."

TOP_N="${1:-0}"
R=report
FAIL=0

if [ "$TOP_N" -gt 0 ]; then
  echo "== pruning feature sets to top $TOP_N"
  python3 - "$TOP_N" <<'PY'
import glob, json, os, sys
n = int(sys.argv[1])
for p in glob.glob('report/features/*/index.json'):
    idx = json.load(open(p))
    d = os.path.dirname(p)
    keep, drop = idx['symbols'][:n], idx['symbols'][n:]
    for s in drop:
        f = os.path.join(d, s['file'])
        if os.path.exists(f): os.remove(f)
    idx['symbols'] = keep
    json.dump(idx, open(p, 'w'), indent=1)
    print(f"   {d}: kept {len(keep)} ({sum(s['bytes'] for s in keep)/1e6:.1f} MB), dropped {len(drop)}")
PY
  # rewrite the dataset manifest to match
  python3 - <<'PY'
import glob, json, sys
sys.path.insert(0, 'tools'); import slice_itch as s
sets = []
for p in sorted(glob.glob('report/features/*/index.json')):
    d = json.load(open(p)); tid = p.split('/')[2]
    sets.append({'id': tid, 'label': s.tape_id(d['tape'])[1], 'path': f'features/{tid}',
                 'tape': d['tape'], 'interval': d['interval'],
                 'symbols_n': len(d['symbols']),
                 'bytes': sum(x['bytes'] for x in d['symbols'])})
sets.sort(key=lambda x: x['id'], reverse=True)
json.dump(sets, open('report/features/manifest.json', 'w'), indent=1)
PY
fi

echo "== required assets"
need(){ if [ -e "$R/$1" ]; then printf '   ok    %s\n' "$1"; else printf '   MISS  %s  (%s)\n' "$1" "$2"; FAIL=1; fi; }
need index.html        "the backtest report"
need live.html         "wasm replay"
need strategy.html     "strategy lab"
need about.html        "performance page"
need theme.css         "shared design tokens"
need favicon.svg       "site mark"
need engine.js         "./wasm/build.sh"
need engine.wasm       "./wasm/build.sh"
need results.json      "orderbook --study all --out report/results.json"
need bench.json        "python3 tools/bench.py <tape>"
need tapes/manifest.json    "tools/make_demo.sh <tape>"
need features/manifest.json "tools/extract.sh <tape>"

echo "== optional / local-only"
[ -e "$R/live-native.html" ] && echo "   ok    live-native.html (ships, but needs a local UDP relay to do anything)"

echo "== CDN limits"
BIG=$(find $R -type f -size +25M | head -5)
if [ -n "$BIG" ]; then echo "   FAIL  over Cloudflare's 25 MiB/file:"; echo "$BIG" | sed 's/^/         /'; FAIL=1;
else echo "   ok    every file under 25 MiB (largest: $(find $R -type f -printf '%s %p\n' | sort -rn | head -1 | awk '{printf "%.1f MB %s", $1/1048576, $2}'))"; fi
N=$(find $R -type f | wc -l)
if [ "$N" -gt 20000 ]; then echo "   FAIL  $N files > 20,000 per deployment"; FAIL=1;
else echo "   ok    $N files (limit 20,000)"; fi

echo "== payload"
python3 - <<'PY'
import os
def size(p):
    if os.path.isfile(p): return os.path.getsize(p)
    return sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs)
rows = [('site (html/css/js/wasm)', sum(size('report/'+f) for f in os.listdir('report')
         if os.path.isfile('report/'+f) and not f.endswith('.json'))),
        ('results.json + bench.json', size('report/results.json') + size('report/bench.json')),
        ('tapes/  (wasm replay)', size('report/tapes')),
        ('features/  (strategy lab)', size('report/features'))]
for k, v in rows: print(f'   {k:32s} {v/1e6:8.1f} MB')
print(f'   {"TOTAL":32s} {sum(v for _, v in rows)/1e6:8.1f} MB')
PY

echo
[ "$FAIL" = 0 ] && echo "== publishable. deploy: see README 'Deploying free'" \
                || { echo "== NOT publishable — fix the items above"; exit 1; }
