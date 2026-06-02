#include <iostream>
#include <algorithm>
#include <unistd.h>

#include "orderbook.hpp"

OrderBook::OrderBook(){
    order_lookup.max_load_factor(0.25);
}

void OrderBook::add_order(uint64_t order_ref, uint32_t price, uint32_t shares, char side){
    order_lookup[order_ref] = {price, shares, side};

    if(side == 'B') bids[price] += shares;
    else asks[price] += shares;
}

void OrderBook::delete_order(uint64_t order_ref){
    auto it = order_lookup.find(order_ref);
    if(it == order_lookup.end()) return;
    
    OrderMeta& order = it -> second;

    if(order.side == 'B') {
        auto pit = bids.find(order.price);
        if(pit != bids.end()){
            pit -> second -= order.shares;
            if(pit -> second == 0) bids.erase(pit);
        }
    }
    
    else{
        auto pit = asks.find(order.price);
        if(pit != asks.end()){
            pit -> second -= order.shares;
            if(pit -> second == 0) asks.erase(pit);
        }
    }
    
    order_lookup.erase(order_ref);
}

bool OrderBook::reduce_order(uint64_t order_ref, uint32_t cancelled_shares){
    auto it = order_lookup.find(order_ref);
    if(it == order_lookup.end()) return true;
    
    OrderMeta& order = it -> second;
    uint32_t remove = std::min(cancelled_shares, order.shares);

    if(order.side == 'B'){
        auto pit = bids.find(order.price);
        if(pit != bids.end()){
            remove = std::min((uint64_t)remove, pit -> second);
            pit -> second -= remove;
            if(pit -> second == 0) bids.erase(pit);
        }
    }
    
    else{
        auto pit = asks.find(order.price);
        if(pit != asks.end()) {
            remove = std::min((uint64_t)remove, pit -> second);
            pit -> second -= remove;
            if(pit -> second == 0) asks.erase(pit);
        }
    }

    order.shares -= remove;
    if(order.shares == 0){
        order_lookup.erase(it);
        return true;
    } 

    return false;
}

void OrderBook::replace_order(uint64_t old_ref, uint64_t new_ref, uint32_t price, uint32_t shares){
    char side = order_lookup[old_ref].side;
    delete_order(old_ref);
    add_order(new_ref, price, shares, side);
}

void OrderBook::fill_snapshot(int levels, const std::string& symbol, uint64_t timestamp_ns, BookSnapshot& snap){
    snap.timestamp_ns = timestamp_ns;
    std::memset(snap.symbol, ' ', 8);
    std::memcpy(snap.symbol, symbol.c_str(), std::min((size_t) 8, symbol.size()));
    
    snap.num_asks = 0;
    for(auto it = asks.begin(); it != asks.end() && snap.num_asks < levels; ++it){
        snap.asks[snap.num_asks].price = it -> first;
        snap.asks[snap.num_asks].shares = it -> second;
        snap.num_asks++;
    }
    
    snap.num_bids = 0;
    for(auto it = bids.begin(); it != bids.end() && snap.num_bids < levels; ++it){
        snap.bids[snap.num_bids].price = it -> first;
        snap.bids[snap.num_bids].shares = it -> second;
        snap.num_bids++;
    }
}