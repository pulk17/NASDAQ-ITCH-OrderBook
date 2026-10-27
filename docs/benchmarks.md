# PitchFork benchmarks

CPU: 13th Gen Intel(R) Core(TM) i7-13650HX  ·  tape: Binaries/S071321-v50.txt  ·  2026-10-27

## Throughput (full tape, median of 5 interleaved runs)

| config | group | M msg/s | min–max | MB/s |
|---|---|--:|--:|--:|
| mmap | io | 11.1762 | 10.9318–12.1974 | 340.209 |
| io_uring | io | 10.7373 | 10.6331–10.8754 | 326.849 |
| chunked 64 KB | io | 11.2051 | 11.1376–11.3382 | 341.088 |
| chunked 1 MB | io | 11.2111 | 11.1471–11.8722 | 341.273 |
| -O0 (no opt) | build | 3.42274 | 3.38602–3.43712 | 104.19 |
| -O2 | build | 11.0178 | 10.1198–11.0296 | 335.387 |
| -O3 +LTO +native | build | 11.2028 | 10.6469–11.2164 | 341.018 |
| all optimizations | engine | 11.0481 | 10.4029–11.1372 | 336.311 |
| std::unordered_map | engine | 8.58362 | 8.3818–8.73577 | 261.29 |
| sorted-vector book | engine | 7.54747 | 7.51886–7.58453 | 229.749 |
| no reserve/tuning | engine | 10.1067 | 10.0514–10.1858 | 307.654 |

Engine toggles each disable one hand-written optimization (same -O3 -march=native). All variants produce the SAME book fingerprint — correctness is unchanged, only speed.

Native vs WASM on the same 22.8M-message slice: 16.37 M msg/s native, 17.33 M msg/s WebAssembly.

## Per-message dispatch latency (rdtsc, warmup discarded)

| mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|--:|--:|--:|--:|--:|--:|--:|
| 150.3 | 127.0 | 260.0 | 446.0 | 778.0 | 5210.0 | 238324.0 | (ns)
