#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <ankerl/unordered_dense.h>

#pragma pack(1)
struct PriceLevel{
    uint32_t price;
    uint32_t shares;
};

struct BookSnapshot{
    uint64_t timestamp_ns;
    int32_t  ofi;
    char     symbol[8];
    uint8_t  num_asks;
    uint8_t  num_bids;
    PriceLevel asks[5];
    PriceLevel bids[5];
};
#pragma pack()

struct OrderMeta{
    uint32_t price;
    uint32_t shares;
    char     side;
};

struct OrderBook{
   ankerl::unordered_dense::map<uint64_t, OrderMeta> order_lookup;
   
   std::vector<PriceLevel> bids;
   std::vector<PriceLevel> asks;
   
   uint32_t prev_bb_price = 0;
   uint32_t prev_bb_shares = 0;
   uint32_t prev_ba_price = 0;
   uint32_t prev_ba_shares = 0;

    OrderBook();
    void add_order(uint64_t order_ref, uint32_t price, uint32_t shares, char side);
    void delete_order(uint64_t order_ref);
    bool reduce_order(uint64_t order_ref, uint32_t cancelled_shares);
    void replace_order(uint64_t old_ref, uint64_t new_ref, uint32_t price, uint32_t shares);
    void fill_snapshot(int levels, const std::string& symbol, uint64_t timestamp_ns, BookSnapshot& snap);
};