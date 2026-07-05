#include <iostream>
#include <algorithm>
#include <unistd.h>

#include "orderbook.hpp"

OrderBook::OrderBook(){
    order_lookup.max_load_factor(0.25);
}

void OrderBook::add_order(uint64_t order_ref, uint32_t price, uint32_t shares, char side){
    order_lookup[order_ref] = {price, shares, side};

    if(side == 'B'){
        auto it = std::lower_bound(bids.begin(), bids.end(), price, [](const PriceLevel& level, uint32_t p) {
            return level.price > p;
        });
        
        if(it != bids.end() && it -> price == price) it -> shares += shares;
        else bids.insert(it, {price, shares});
    }
    
    else{
        auto it = std::lower_bound(asks.begin(), asks.end(), price, [](const PriceLevel& level, uint32_t p) {
            return level.price < p;
        });
        
        if(it != asks.end() && it -> price == price) it -> shares += shares;
        else asks.insert(it, {price, shares});
    }

    update_ofi();
}

void OrderBook::delete_order(uint64_t order_ref){
    auto it = order_lookup.find(order_ref);
    if(it == order_lookup.end()) return;
    
    OrderMeta& order = it -> second;

    if(order.side == 'B') {
        auto pit = std::lower_bound(bids.begin(), bids.end(), order.price, [](const PriceLevel& level, uint32_t p) {
            return level.price > p;
        });
        
        if(pit != bids.end() && pit -> price == order.price){
            pit -> shares -= order.shares;
            if(pit -> shares == 0) bids.erase(pit);
        }
    }
    
    else{
        auto pit = std::lower_bound(asks.begin(), asks.end(), order.price, [](const PriceLevel& level, uint32_t p) {
            return level.price < p;
        });
        
        if(pit != asks.end() && pit -> price == order.price){
            pit -> shares -= order.shares;
            if(pit -> shares == 0) asks.erase(pit);
        }
    }
    
    update_ofi();
    order_lookup.erase(order_ref);
}

bool OrderBook::reduce_order(uint64_t order_ref, uint32_t cancelled_shares){
    auto it = order_lookup.find(order_ref);
    if(it == order_lookup.end()) return true;
    
    OrderMeta& order = it -> second;
    uint32_t remove = std::min(cancelled_shares, order.shares);

    if(order.side == 'B'){
        auto pit = std::lower_bound(bids.begin(), bids.end(), order.price, [](const PriceLevel& level, uint32_t p) {
            return level.price > p;
        });
        
        if(pit != bids.end() && pit -> price == order.price){
            uint32_t actual_remove = std::min(remove, pit -> shares);
            pit -> shares -= actual_remove;
            if(pit -> shares == 0) bids.erase(pit);
        }
    }
    
    else{
        auto pit = std::lower_bound(asks.begin(), asks.end(), order.price, [](const PriceLevel& level, uint32_t p) {
            return level.price < p;
        });
        
        if(pit != asks.end() && pit -> price == order.price) {
            uint32_t actual_remove = std::min(remove, pit -> shares);
            pit -> shares -= actual_remove;
            if(pit -> shares == 0) asks.erase(pit);
        }
    }

    update_ofi();

    order.shares -= remove;
    if(order.shares == 0){
        order_lookup.erase(it);
        return true;
    } 

    return false;
}

void OrderBook::replace_order(uint64_t old_ref, uint64_t new_ref, uint32_t price, uint32_t shares){
    auto it = order_lookup.find(old_ref);
    if(it == order_lookup.end()) return;
    
    char side = it -> second.side;
    delete_order(old_ref);
    add_order(new_ref, price, shares, side);
}

void OrderBook::update_ofi(){
    uint32_t current_bb_price  = bids.empty() ? 0 : bids[0].price;
    uint32_t current_bb_shares = bids.empty() ? 0 : bids[0].shares;
    uint32_t current_ba_price  = asks.empty() ? 0 : asks[0].price;
    uint32_t current_ba_shares = asks.empty() ? 0 : asks[0].shares;

    int32_t bid_flux = 0;
    if(current_bb_price > prev_bb_price) bid_flux = (int32_t)current_bb_shares;
    else if(current_bb_price == prev_bb_price) bid_flux = (int32_t)current_bb_shares - (int32_t)prev_bb_shares;
    else bid_flux = -(int32_t)prev_bb_shares;

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
    for(size_t i = 0; i < bids.size() && snap.num_bids < levels; ++ i){
        snap.bids[snap.num_bids] = bids[i];
        snap.num_bids++;
    }
}