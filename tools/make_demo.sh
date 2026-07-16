#!/usr/bin/env bash
# Build (or rebuild) every browser asset for one ITCH tape, then refresh the
# multi-day manifest. Idempotent: safe to re-run, safe to run per tape.
#
#   tools/make_demo.sh <tape> [TOPN=80] [RAW_MB=240] [CUT=09:10] [--variant NAME] [--study] [--host URL]
#
# --variant  suffix the dataset id, so one tape can ship at several sizes side by side
#            (e.g. a 20 MB "lite" cut next to the 222 MB full session). live.html shows
#            a picker as soon as the manifest has more than one entry.
# --study  also runs the native cross-sectional study into report/results-<id>.json
# --host   rewrites meta.json so chunks after the first load from URL/<id>/ (for
#          GitHub Releases / Cloudflare R2); chunk 0 stays local for instant start.
set -euo pipefail
cd "$(dirname "$0")/.."

TAPE="${1:?usage: make_demo.sh <tape> [TOPN] [RAW_MB] [CUT] [--study] [--host URL]}"
TOPN="${2:-80}"; RAW_MB="${3:-240}"; CUT="${4:-09:10}"
STUDY=0; HOST=""; VARIANT=""
shift $(( $# < 4 ? $# : 4 )) || true
while [ $# -gt 0 ]; do
  case "$1" in
    --study)   STUDY=1 ;;
    --host)    HOST="$2"; shift ;;
    --variant) VARIANT="$2"; shift ;;
  esac
  shift
done

ID=$(python3 -c "import sys;sys.path.insert(0,'tools');import slice_itch as s;print(s.tape_id('$TAPE')[0])")
[ -n "$VARIANT" ] && ID="$ID-$VARIANT"
OUT="report/tapes/$ID"
echo "== tape $TAPE -> $OUT (TOP$TOPN, ${RAW_MB}MB, cut $CUT${VARIANT:+, variant $VARIANT})"

# 1. activity ranking (regenerate if missing or older than the tape)
if [ ! -f tools/ranking.txt ] || [ tools/ranking.txt -ot "$TAPE" ]; then
  echo "-- ranking symbols (full-tape scan)"
  python3 tools/rank_symbols.py "$TAPE" > tools/ranking.txt
fi

# 2. slice into gzip chunks + meta.json
python3 tools/slice_itch.py "$TAPE" "$OUT" "TOP$TOPN" "$RAW_MB" "$CUT"

# 2b. the slicer keys meta off the tape date; re-stamp it so the variant is addressable
if [ -n "$VARIANT" ]; then
  python3 - "$OUT/meta.json" "$ID" "$VARIANT" <<'PY'
import json, sys
path, tid, var = sys.argv[1:4]
meta = json.load(open(path))
meta['id'] = tid
meta['variant'] = var
meta['label'] = f"{meta['label']} · {var}"
json.dump(meta, open(path, 'w'), indent=1)
PY
fi

# 3. rebuild the wasm engine if any source is newer than the artifact
if [ ! -f report/engine.wasm ] || \
   [ -n "$(find wasm/bindings.cpp include src/engine.cpp src/orderbook.cpp -newer report/engine.wasm 2>/dev/null)" ]; then
  echo "-- rebuilding wasm"
  ./wasm/build.sh
fi

# 4. optional external hosting: chunks after the first fetch from HOST/<id>/
if [ -n "$HOST" ]; then
  python3 - "$OUT/meta.json" "$HOST" "$ID" <<'PY'
import json, sys
meta_path, host, tid = sys.argv[1:4]
meta = json.load(open(meta_path))
for i, c in enumerate(meta['chunks']):
    if i == 0:
        c.pop('url', None)
    else:
        c['url'] = f"{host.rstrip('/')}/{tid}/{c['file']}"
json.dump(meta, open(meta_path, 'w'), indent=1)
print(f"  hosted chunks 1..{len(meta['chunks'])-1} at {host}/{tid}/")
PY
fi

# 5. rebuild the manifest from every meta.json present
python3 - <<'PY'
import glob, json, os
days = []
for m in sorted(glob.glob('report/tapes/*/meta.json')):
    d = json.load(open(m))
    gz = sum(c['gz_bytes'] for c in d['chunks'])
    days.append({'id': d['id'], 'label': d['label'], 'path': f"tapes/{d['id']}",
                 'gz_bytes': gz, 'span': d['span'], 'symbols_n': len(d['symbols']),
                 'tape': d.get('tape', '')})
# Newest day first; within a day the smallest variant first, so live.html's default
# (days[0]) is always the fastest thing to load rather than a 222 MB session.
days.sort(key=lambda x: (x['id'][:10], -x['gz_bytes']), reverse=True)
json.dump(days, open('report/tapes/manifest.json', 'w'), indent=1)
print(f"  manifest: {len(days)} day(s) -> {', '.join(x['id'] for x in days)}")
PY

# 6. optional native study
if [ "$STUDY" = 1 ]; then
  echo "-- native study -> report/results-$ID.json"
  ./build/orderbook "$TAPE" --study all \
    --detail AAPL,AMZN,MSFT,NVDA,SPY,TSLA --interval 50 --out "report/results-$ID.json"
  cp "report/results-$ID.json" report/results.json   # newest is the default
fi

echo "== done. verify: node tools/verify.js $ID"
