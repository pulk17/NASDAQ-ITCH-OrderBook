#!/usr/bin/env bash
# Build the benchmark comparison binaries: optimization levels + engine-toggle
# variants (one hand-written optimization disabled at a time). All land in build/
# (gitignored). Run tools/bench.py afterwards.
set -e
cd "$(dirname "$0")/.."
INC="-Iinclude -Ithird_party/unordered_dense/include"
SRC="src/main.cpp src/orderbook.cpp src/engine.cpp"
LNK="-lpthread -luring"

echo "optimization levels…"
g++ -O0 -std=c++20 -DGIT_COMMIT='"o0"' $INC $SRC -o build/orderbook_o0 $LNK
g++ -O2 -std=c++20 -DGIT_COMMIT='"o2"' $INC $SRC -o build/orderbook_o2 $LNK

echo "engine toggles (-O3 -march=native, one optimization off each)…"
F="-O3 -march=native -std=c++20"
g++ $F               $INC $SRC -o build/ob_base       $LNK   # all on
g++ $F -DPF_STD_MAP  $INC $SRC -o build/ob_stdmap     $LNK   # std::unordered_map, not unordered_dense
g++ $F -DPF_SPAN=8   $INC $SRC -o build/ob_noladder   $LNK   # tick ladder off -> sorted-vector fallback
g++ $F -DPF_NO_RESERVE $INC $SRC -o build/ob_noreserve $LNK  # no reserve / load-factor tuning
echo "done -> build/{orderbook_o0,orderbook_o2,ob_base,ob_stdmap,ob_noladder,ob_noreserve}"
