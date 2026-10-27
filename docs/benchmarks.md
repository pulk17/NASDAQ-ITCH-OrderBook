# PitchFork benchmarks

CPU: 13th Gen Intel(R) Core(TM) i7-13650HX  ·  tape: Binaries/S071321-v50.txt  ·  2026-10-27

## Throughput (full tape, median of 5 interleaved runs)

| config | group | M msg/s | min–max | MB/s |
|---|---|--:|--:|--:|
| mmap | io | 9.40841 | 8.96295–9.93447 | 286.397 |
| io_uring | io | 9.43426 | 8.69484–9.45475 | 287.184 |
| chunked 64 KB | io | 9.41311 | 8.72522–9.89697 | 286.54 |
| chunked 1 MB | io | 9.70694 | 9.01497–9.85842 | 295.485 |
| -O0 (no opt) | build | 3.30978 | 3.09664–3.40174 | 100.752 |
| -O2 | build | 9.36363 | 8.63876–9.68652 | 285.034 |
| -O3 +LTO +native | build | 9.46869 | 8.26735–9.83758 | 288.232 |
| all optimizations | engine | 9.58865 | 9.05984–10.1837 | 291.884 |
| std::unordered_map | engine | 7.65267 | 7.37108–8.63079 | 232.952 |
| sorted-vector book | engine | 6.71648 | 6.53194–7.1989 | 204.453 |
| no reserve/tuning | engine | 8.63162 | 8.36712–9.34206 | 262.751 |

Engine toggles each disable one hand-written optimization (same -O3 -march=native). All variants produce the SAME book fingerprint — correctness is unchanged, only speed.

Native vs WASM on the same 22.8M-message slice: 16.37 M msg/s native, 17.33 M msg/s WebAssembly.

## Per-message dispatch latency (rdtsc, warmup discarded)

| mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|--:|--:|--:|--:|--:|--:|--:|
| 171.6 | 140.0 | 304.0 | 510.0 | 820.0 | 4587.0 | 225022.0 | (ns)
