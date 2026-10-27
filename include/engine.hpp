#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <memory>

#include "orderbook.hpp"
#include "messages.hpp"

// Order/symbol maps. Build with -DPF_STD_MAP to swap the flat unordered_dense
// map for std::unordered_map (measure the hashmap's value); -DPF_NO_RESERVE to
// drop the reserve + load-factor tuning.
#ifdef PF_STD_MAP
  #include <unordered_map>
  template<class K, class V> using PFMap = std::unordered_map<K, V>;
#else
  #include <ankerl/unordered_dense.h>
  template<class K, class V> using PFMap = ankerl::unordered_dense::map<K, V>;
#endif

struct Engine {
    std::unique_ptr<OrderBook[]> books;
    PFMap<uint64_t, OrderInfo> orders;
    std::array<std::string, 65536> locate_to_symbol;
    PFMap<std::string, uint16_t> symbol_to_locate;

    // Order-flow imbalance is research state, not book state: only the study,
    // feature extraction, live stream and WASM pages read it. Off, the hot path
    // is parse -> mutate -> done.
    bool track_ofi = true;

    explicit Engine(size_t reserve_orders = 8000000) : books(std::make_unique<OrderBook[]>(65536)) {
#ifndef PF_NO_RESERVE
        orders.max_load_factor(0.5);
        orders.reserve(reserve_orders);
#endif
    }

    uint16_t process_message(const char* buffer);

    // FNV-1a over every level of every book, folded into `h`. Native and WASM
    // share this, so equal hashes mean byte-identical book state.
    static constexpr uint64_t FNV_BASIS = 14695981039346656037ULL;
    uint64_t fingerprint(uint64_t h = FNV_BASIS) const;

private:
    void     book_add(uint16_t locate, uint32_t price, uint32_t shares, char side);
    void     book_remove(uint16_t locate, uint32_t price, uint32_t shares, char side);
    uint16_t reduce(uint64_t order_ref, uint32_t shares);
};