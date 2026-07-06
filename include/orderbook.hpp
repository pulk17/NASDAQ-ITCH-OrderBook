#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

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

struct OrderInfo{
    uint32_t price;
    uint32_t shares;
    uint16_t locate;
    char     side;
};

struct OrderBook{
    std::vector<PriceLevel> bids;   // descending by price (best = front)
    std::vector<PriceLevel> asks;   // ascending  by price (best = front)

    uint32_t prev_bb_price = 0;
    uint32_t prev_bb_shares = 0;
    uint32_t prev_ba_price = 0;
    uint32_t prev_ba_shares = 0;
    int64_t  ofi_accumulator = 0;

    void add(uint32_t price, uint32_t shares, char side);
    void remove(uint32_t price, uint32_t shares, char side);
    void fill_snapshot(int levels, const std::string& symbol, uint64_t timestamp_ns, BookSnapshot& snap);

private:
    void update_ofi();
};