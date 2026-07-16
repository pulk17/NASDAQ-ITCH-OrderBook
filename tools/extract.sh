#!/usr/bin/env bash
# Sweep an ITCH tape with the native engine and dump Strategy Lab features for every
# symbol on it — no slicing. Writes report/features/<tape-id>/ and refreshes the
# dataset manifest, so several tapes can live side by side and report/strategy.html
# offers a picker. Idempotent: safe to re-run, safe to run per tape.
#
#   tools/extract.sh <tape> [TOP_N=0] [INTERVAL=50]
#
#   tools/extract.sh S071321-v50.txt          # every symbol (~100s, ~330 MB, 3071 files)
#   tools/extract.sh S071321-v50.txt 250      # …then keep only the 250 most active
#   tools/extract.sh S071321-v50.txt 250 100  # …sampling every 100 messages instead of 50
#
# TOP_N=0 keeps everything. The Lab lazy-fetches one symbol at a time, so a visitor only
# downloads what they click (median symbol ~52 KB, largest ~5 MB) — but the whole set is
# ~330 MB on disk, so prune to a top-N you are willing to commit before deploying.
set -euo pipefail
cd "$(dirname "$0")/.."

TAPE="${1:?usage: tools/extract.sh <tape> [TOP_N] [INTERVAL]}"
TOP_N="${2:-0}"
IVL="${3:-50}"

[ -x build/orderbook ] || { echo "build/orderbook missing — cmake -B build && cmake --build build -j"; exit 1; }
[ -f "$TAPE" ] || { echo "no such tape: $TAPE"; exit 1; }

# same id convention as make_demo.sh: S071321-v50.txt -> 2021-07-13
ID=$(python3 -c "import sys;sys.path.insert(0,'tools');import slice_itch as s;print(s.tape_id('$TAPE')[0])")
OUT="report/features/$ID"
echo "== tape $TAPE -> $OUT (interval $IVL)"

./build/orderbook "$TAPE" --extract all --interval "$IVL" --min-samples 500 --features-out "$OUT"

if [ "$TOP_N" -gt 0 ]; then
  echo "-- pruning to the top $TOP_N by sample count"
  python3 - "$OUT" "$TOP_N" <<'PY'
import json, os, sys
out, n = sys.argv[1], int(sys.argv[2])
idx = json.load(open(f'{out}/index.json'))
keep, drop = idx['symbols'][:n], idx['symbols'][n:]
for s in drop:
    os.remove(os.path.join(out, s['file']))
idx['symbols'] = keep
json.dump(idx, open(f'{out}/index.json', 'w'), indent=1)
print(f"   kept {len(keep)} ({sum(s['bytes'] for s in keep)/1e6:.1f} MB), dropped {len(drop)}")
PY
fi

# rebuild the dataset manifest from every index.json present
python3 - <<'PY'
import glob, json, sys
sys.path.insert(0, 'tools')
import slice_itch as s
sets = []
for p in sorted(glob.glob('report/features/*/index.json')):
    d = json.load(open(p))
    tid = p.split('/')[2]
    sets.append({'id': tid, 'label': s.tape_id(d['tape'])[1], 'path': f'features/{tid}',
                 'tape': d['tape'], 'interval': d['interval'],
                 'symbols_n': len(d['symbols']),
                 'bytes': sum(x['bytes'] for x in d['symbols'])})
sets.sort(key=lambda x: x['id'], reverse=True)
json.dump(sets, open('report/features/manifest.json', 'w'), indent=1)
print(f"   manifest: {len(sets)} dataset(s) -> " + ', '.join(f"{x['id']} ({x['symbols_n']} syms)" for x in sets))
PY

du -sh "$OUT"
echo "== done. serve: cd report && python3 -m http.server 8000  ->  /strategy.html"
