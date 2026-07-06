#include <algorithm>
#include <cassert>
#include <cstring>

#include "orderbook.hpp"

void OrderBook::add(uint32_t price, uint32_t shares, char side){
    if(side == 'B'){
        auto it = std::lower_bound(bids.begin(), bids.end(), price, [](const PriceLevel& level, uint32_t p){
            return level.price > p;                 // bids: descending
        });
        if(it != bids.end() && it->price == price) it->shares += shares;
        else bids.insert(it, {price, shares});
    } else {
        auto it = std::lower_bound(asks.begin(), asks.end(), price, [](const PriceLevel& level, uint32_t p){
            return level.price < p;                 // asks: ascending
        });
        if(it != asks.end() && it->price == price) it->shares += shares;
        else asks.insert(it, {price, shares});
    }
    update_ofi();
}

void OrderBook::remove(uint32_t price, uint32_t shares, char side){
    if(side == 'B'){
        auto pit = std::lower_bound(bids.begin(), bids.end(), price, [](const PriceLevel& level, uint32_t p){
            return level.price > p;
        });
        if(pit != bids.end() && pit->price == price){
            assert(shares <= pit->shares && "remove: bid level < shares (book desync)");
            uint32_t r = std::min(shares, pit->shares);
            pit->shares -= r;
            if(pit->shares == 0) bids.erase(pit);
        } else {
            assert(false && "remove: bid level missing for a live order (book desync)");
        }
    } else {
        auto pit = std::lower_bound(asks.begin(), asks.end(), price, [](const PriceLevel& level, uint32_t p){
            return level.price < p;
        });
        if(pit != asks.end() && pit->price == price){
            assert(shares <= pit->shares && "remove: ask level < shares (book desync)");
            uint32_t r = std::min(shares, pit->shares);
            pit->shares -= r;
            if(pit->shares == 0) asks.erase(pit);
        } else {
            assert(false && "remove: ask level missing for a live order (book desync)");
        }
    }
    update_ofi();
}

void OrderBook::update_ofi(){
    uint32_t current_bb_price  = bids.empty() ? 0 : bids[0].price;
    uint32_t current_bb_shares = bids.empty() ? 0 : bids[0].shares;
    uint32_t current_ba_price  = asks.empty() ? 0 : asks[0].price;
    uint32_t current_ba_shares = asks.empty() ? 0 : asks[0].shares;

    // Bid side:  up -> +new size ; equal -> size delta ; down -> -old size
    int32_t bid_flux = 0;
    if(current_bb_price > prev_bb_price) bid_flux = (int32_t)current_bb_shares;
    else if(current_bb_price == prev_bb_price) bid_flux = (int32_t)current_bb_shares - (int32_t)prev_bb_shares;
    else bid_flux = -(int32_t)prev_bb_shares;

    // Ask side: mirror (improving direction is DOWN):
    //   down -> +new size ; equal -> size delta ; up -> -old size
    int32_t ask_flux = 0;
    if(current_ba_price < prev_ba_price) ask_flux = (int32_t)current_ba_shares;
    else if(current_ba_price == prev_ba_price) ask_flux = (int32_t)current_ba_shares - (int32_t)prev_ba_shares;
    else ask_flux = -(int32_t)prev_ba_shares;

    ofi_accumulator += (int64_t)bid_flux - (int64_t)ask_flux;

    prev_bb_price = current_bb_price;
    prev_bb_shares = current_bb_shares;
    prev_ba_price = current_ba_price;
    prev_ba_shares = current_ba_shares;
}

void OrderBook::fill_snapshot(int levels, const std::string& symbol, uint64_t timestamp_ns, BookSnapshot& snap){
    snap.timestamp_ns = timestamp_ns;
    std::memset(snap.symbol, ' ', 8);
    std::memcpy(snap.symbol, symbol.c_str(), std::min((size_t) 8, symbol.size()));

    snap.ofi = (int32_t)ofi_accumulator;
    ofi_accumulator = 0;

    snap.num_asks = 0;
    for(size_t i = 0; i < asks.size() && snap.num_asks < levels; ++i){
        snap.asks[snap.num_asks] = asks[i];
        snap.num_asks++;
    }

    snap.num_bids = 0;
    for(size_t i = 0; i < bids.size() && snap.num_bids < levels; ++i){
        snap.bids[snap.num_bids] = bids[i];
        snap.num_bids++;
    }
}