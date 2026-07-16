# PitchFork benchmarks

CPU: 13th Gen Intel(R) Core(TM) i7-13650HX  ·  tape: S071321-v50.txt  ·  2026-07-16

## Throughput (plain run, full tape)

| config | group | M msg/s | MB/s |
|---|---|--:|--:|
| mmap | io | 8.52505 | 259.507 |
| io_uring | io | 8.46302 | 257.619 |
| chunked 64 KB | io | 8.88381 | 270.428 |
| chunked 1 MB | io | 8.81223 | 268.249 |
| -O0 (no opt) | build | 2.98223 | 90.7807 |
| -O2 | build | 8.7151 | 265.293 |
| -O3 +LTO +native | build | 8.9072 | 271.14 |
| all optimizations | engine | 8.87349 | 270.114 |
| std::unordered_map | engine | 7.39309 | 225.05 |
| sorted-vector book | engine | 6.34091 | 193.021 |
| no reserve/tuning | engine | 8.34512 | 254.03 |

Engine toggles each disable one hand-written optimization (same -O3 -march=native). All variants produce the SAME book fingerprint — correctness is unchanged, only speed.

Native vs WASM on the same 22.8M-message slice: 16.37 M msg/s native, 17.33 M msg/s WebAssembly.

## Per-message dispatch latency (rdtsc, warmup discarded)

| mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|--:|--:|--:|--:|--:|--:|--:|
| 172.5 | 153.0 | 285.0 | 458.0 | 760.0 | 3811.0 | 196172.0 | (ns)
