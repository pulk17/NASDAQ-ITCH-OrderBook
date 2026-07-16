#!/usr/bin/env python3
"""Benchmark the engine across I/O sources and optimization levels.

Throughput is measured with a plain run (no instrumentation); per-message
dispatch latency is measured once with --bench (rdtsc adds overhead, so it is
kept out of the throughput numbers, and dispatch latency is I/O-independent).
Writes report/bench.json (About page) and docs/benchmarks.md (design doc),
incrementally. Run with the device in full-performance mode.

The -O0/-O2 build variants (optimization axis) are optional; build them with:
  g++ -O0 -std=c++20 -Iinclude -Ithird_party/unordered_dense/include \\
      src/main.cpp src/orderbook.cpp src/engine.cpp -o build/orderbook_o0 -lpthread -luring
  g++ -O2 …                                                        -o build/orderbook_o2 …
Missing variants are skipped.

    python3 tools/bench.py S071321-v50.txt
"""
import json
import os
import platform
import re
import subprocess
import sys
import time

TAPE = sys.argv[1] if len(sys.argv) > 1 else 'S071321-v50.txt'
# same-workload native-vs-WASM point (22.8M-msg TOP6 slice), measured separately
WASM_MPS = 17.33
NATIVE_SUBSET_MPS = 16.37

# (label, group, binary, extra args) — plain runs, throughput only.
# The 'engine' group toggles one hand-written optimization off at a time, all at
# -O3 -march=native (no LTO); ob_base is that group's all-on baseline. Build them
# with tools/build_variants.sh.
CONFIGS = [
    ('mmap',             'io',     'build/orderbook',    []),
    ('io_uring',         'io',     'build/orderbook',    ['--io_uring']),
    ('chunked 64 KB',    'io',     'build/orderbook',    ['--chunk', '65536']),
    ('chunked 1 MB',     'io',     'build/orderbook',    ['--chunk', '1048576']),
    ('-O0 (no opt)',     'build',  'build/orderbook_o0', []),
    ('-O2',              'build',  'build/orderbook_o2', []),
    ('-O3 +LTO +native', 'build',  'build/orderbook',    []),
    ('all optimizations', 'engine', 'build/ob_base',      []),
    ('std::unordered_map', 'engine', 'build/ob_stdmap',   []),
    ('sorted-vector book', 'engine', 'build/ob_noladder', []),
    ('no reserve/tuning', 'engine', 'build/ob_noreserve', []),
]
N = r'([\d.]+)'
P = {'mps': re.compile(r'Throughput:\s*'+N), 'mb_s': re.compile(r'Ingestion:\s*'+N),
     'seconds': re.compile(r'in\s*'+N+r's'), 'messages': re.compile(r'Processed\s*(\d+)'),
     'fingerprint': re.compile(r'fingerprint:\s*(\d+)', re.I)}
LP = {k: re.compile(k.replace('.', r'\.')+r'\s*:\s*'+N) for k in
      ['mean', 'p50', 'p90', 'p99', 'p99.9', 'p99.99', 'max']}


def run(binary, extra):
    out = subprocess.run(['./'+binary, TAPE]+extra, capture_output=True, text=True).stdout
    return out


def main():
    rows, latency = [], {}
    for label, group, binary, extra in CONFIGS:
        if not os.path.exists(binary):     # e.g. build the -O0/-O2 variants first (see docs)
            print(f'-- {label} … skipped (missing {binary})', flush=True)
            continue
        print(f'-- {label} …', flush=True)
        t = time.time()
        out = run(binary, extra)
        row = {'label': label, 'group': group, 'wall_s': round(time.time()-t, 1)}
        for k, pat in P.items():
            m = pat.search(out)
            row[k] = (m.group(1) if k == 'fingerprint' else float(m.group(1))) if m else None
        rows.append(row)
        print(f'   {row.get("mps")} M msg/s · {row.get("mb_s")} MB/s  ({row["wall_s"]}s)', flush=True)
        write(rows, latency)

    print('-- latency (--bench, mmap) …', flush=True)
    out = run('build/orderbook', ['--bench'])
    for k, pat in LP.items():
        m = pat.search(out)
        latency[k] = float(m.group(1)) if m else None
    fp = re.search(r'fingerprint:\s*(\d+)', out, re.I)
    latency['fingerprint'] = fp.group(1) if fp else None
    print(f'   p50 {latency.get("p50")} ns · p99 {latency.get("p99")} ns · max {latency.get("max")} ns', flush=True)
    write(rows, latency)
    print('done -> report/bench.json, docs/benchmarks.md', flush=True)


def write(rows, latency):
    cpu = next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo')
                if l.startswith('model name')), platform.processor())
    meta = {'cpu': cpu, 'tape': TAPE, 'date': time.strftime('%Y-%m-%d'),
            'wasm_mps': WASM_MPS, 'native_subset_mps': NATIVE_SUBSET_MPS, 'latency': latency}
    json.dump({'meta': meta, 'runs': rows}, open('report/bench.json', 'w'), indent=1)

    with open('docs/benchmarks.md', 'w') as f:
        f.write(f'# PitchFork benchmarks\n\nCPU: {cpu}  ·  tape: {TAPE}  ·  {meta["date"]}\n\n')
        f.write('## Throughput (plain run, full tape)\n\n| config | group | M msg/s | MB/s |\n|---|---|--:|--:|\n')
        for r in rows:
            f.write(f'| {r["label"]} | {r["group"]} | {r.get("mps")} | {r.get("mb_s")} |\n')
        eng = [r for r in rows if r['group'] == 'engine']
        if eng:
            fps = {r.get('fingerprint') for r in rows if r.get('fingerprint')}
            f.write(f'\nEngine toggles each disable one hand-written optimization (same -O3 -march=native). '
                    f'All variants produce {"the SAME" if len(fps)==1 else "DIFFERENT (!)"} book fingerprint '
                    f'— correctness is unchanged, only speed.\n')
        f.write(f'\nNative vs WASM on the same 22.8M-message slice: '
                f'{NATIVE_SUBSET_MPS} M msg/s native, {WASM_MPS} M msg/s WebAssembly.\n\n')
        if latency:
            f.write('## Per-message dispatch latency (rdtsc, warmup discarded)\n\n')
            f.write('| mean | p50 | p90 | p99 | p99.9 | p99.99 | max |\n|--:|--:|--:|--:|--:|--:|--:|\n')
            f.write('| ' + ' | '.join(str(latency.get(k)) for k in
                    ['mean', 'p50', 'p90', 'p99', 'p99.9', 'p99.99', 'max']) + ' | (ns)\n')


if __name__ == '__main__':
    main()
