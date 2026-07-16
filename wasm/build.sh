#!/bin/bash
set -e
cd "$(dirname "$0")/.."
em++ -O3 -std=c++20 \
  -I include -I third_party/unordered_dense/include \
  src/engine.cpp src/orderbook.cpp wasm/bindings.cpp \
  -s WASM=1 -s MODULARIZE=1 -s EXPORT_NAME=PitchFork \
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=33554432 \
  -s EXPORTED_FUNCTIONS='["_engine_create","_engine_feed","_engine_feed_until","_engine_msg_count","_engine_symbol_count","_engine_locate","_engine_levels","_engine_ofi","_engine_fingerprint","_engine_last_ts","_engine_symbol_at","_engine_active_at","_engine_locate_msgs","_strategy_stat","_strategy_set_interval","_strategy_set_latency","_trade_count","_trade_get","_malloc","_free"]' \
  -s EXPORTED_RUNTIME_METHODS='["ccall","HEAPU8","HEAPU32","stringToUTF8","UTF8ToString"]' \
  -o report/engine.js
echo "built report/engine.js + report/engine.wasm"
ls -lh report/engine.wasm