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
struct BookSide{
    static constexpr uint32_t SPAN = 2048;

    std::vector<uint32_t> ladder;
    uint32_t base_tick   = 0;
    uint32_t best_tick   = 0;
    bool     best_valid  = false;
    uint32_t nonzero     = 0;
    std::vector<PriceLevel> fallback;

    bool anchored() const { return !ladder.empty(); }
    bool in_window(uint32_t tick) const { return tick >= base_tick && tick < base_tick + SPAN; }
};

struct OrderBook{
    BookSide bid;
    BookSide ask;

    uint32_t prev_bb_price = 0;
    uint32_t prev_bb_shares = 0;
    uint32_t prev_ba_price = 0;
    uint32_t prev_ba_shares = 0;
    int64_t  ofi_accumulator = 0;

    uint64_t reanchor_count = 0;
    uint64_t fallback_ops   = 0;

    void add(uint32_t price, uint32_t shares, char side);
    void remove(uint32_t price, uint32_t shares, char side);
    void fill_snapshot(int levels, const std::string& symbol, uint64_t timestamp_ns, BookSnapshot& snap);

    PriceLevel best_bid() const;
    PriceLevel best_ask() const;

    std::vector<PriceLevel> all_bids() const;
    std::vector<PriceLevel> all_asks() const;

private:
    void side_add(BookSide& s, bool is_bid, uint32_t price, uint32_t shares);
    void side_remove(BookSide& s, bool is_bid, uint32_t price, uint32_t shares);
    void rescan_best(BookSide& s, bool is_bid);
    void reanchor(BookSide& s, bool is_bid, uint32_t center_tick);
    void maybe_reanchor(BookSide& s, bool is_bid);
    void collect(const BookSide& s, bool is_bid, size_t max_levels, std::vector<PriceLevel>& out) const;
    void update_ofi();
};