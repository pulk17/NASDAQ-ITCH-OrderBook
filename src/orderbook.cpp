#include <algorithm>
#include <cassert>
#include <cstring>

#include "orderbook.hpp"

static inline void fallback_insert(std::vector<PriceLevel>& v, bool is_bid,
                                   uint32_t price, uint32_t shares){
    auto it = is_bid
        ? std::lower_bound(v.begin(), v.end(), price,
              [](const PriceLevel& l, uint32_t p){ return l.price > p; })
        : std::lower_bound(v.begin(), v.end(), price,
              [](const PriceLevel& l, uint32_t p){ return l.price < p; });
    if(it != v.end() && it->price == price) it->shares += shares;
    else v.insert(it, {price, shares});
}

static inline void fallback_remove(std::vector<PriceLevel>& v, bool is_bid,
                                   uint32_t price, uint32_t shares){
    auto it = is_bid
        ? std::lower_bound(v.begin(), v.end(), price,
              [](const PriceLevel& l, uint32_t p){ return l.price > p; })
        : std::lower_bound(v.begin(), v.end(), price,
              [](const PriceLevel& l, uint32_t p){ return l.price < p; });
    if(it != v.end() && it->price == price){
        assert(shares <= it->shares && "fallback: level < shares (book desync)");
        uint32_t r = std::min(shares, it->shares);
        it->shares -= r;
        if(it->shares == 0) v.erase(it);
    } else {
        assert(false && "fallback: level missing for a live order (book desync)");
    }
}

void OrderBook::reanchor(BookSide& s, bool is_bid, uint32_t center_tick){
    reanchor_count++;
    uint32_t new_base = (center_tick > BookSide::SPAN/2) ? center_tick - BookSide::SPAN/2 : 0;

    std::vector<PriceLevel> tmp;
    tmp.reserve(s.nonzero + s.fallback.size());
    if(s.anchored())
        for(uint32_t i = 0; i < BookSide::SPAN; i++)
            if(s.ladder[i]) tmp.push_back({(s.base_tick + i) * 100, s.ladder[i]});
    for(const auto& lvl : s.fallback) tmp.push_back(lvl);

#ifndef NDEBUG
    uint64_t total_before = 0;
    for(const auto& l : tmp) total_before += l.shares;
#endif

    s.fallback.clear();
    if(s.ladder.empty()) s.ladder.assign(BookSide::SPAN, 0);
    else std::fill(s.ladder.begin(), s.ladder.end(), 0);
    s.base_tick = new_base;
    s.nonzero = 0;
    s.best_valid = false;

    std::vector<PriceLevel> out;
    for(const auto& lvl : tmp){
        uint32_t tick = lvl.price / 100;
        if(lvl.price % 100 == 0 && s.in_window(tick)){
            s.ladder[tick - s.base_tick] = lvl.shares;
            s.nonzero++;
            if(!s.best_valid || (is_bid ? tick > s.best_tick : tick < s.best_tick)){
                s.best_tick = tick; s.best_valid = true;
            }
        } else {
            out.push_back(lvl);
        }
    }
    if(is_bid) std::sort(out.begin(), out.end(), [](const PriceLevel& a, const PriceLevel& b){ return a.price > b.price; });
    else       std::sort(out.begin(), out.end(), [](const PriceLevel& a, const PriceLevel& b){ return a.price < b.price; });
    s.fallback = std::move(out);

#ifndef NDEBUG
    uint64_t total_after = 0;
    if(s.anchored()) for(uint32_t i = 0; i < BookSide::SPAN; i++) total_after += s.ladder[i];
    for(const auto& l : s.fallback) total_after += l.shares;
    assert(total_before == total_after && "reanchor lost shares");
#endif
}

void OrderBook::maybe_reanchor(BookSide& s, bool is_bid){
    uint32_t fb_price = 0; bool fb_valid = false;
    if(!s.fallback.empty()){ fb_price = s.fallback[0].price; fb_valid = true; }

    if(fb_valid && fb_price % 100 == 0){
        bool fb_better = !s.best_valid ||
            (is_bid ? fb_price/100 > s.best_tick : fb_price/100 < s.best_tick);
        if(fb_better){ reanchor(s, is_bid, fb_price/100); return; }
    }
    if(s.best_valid){
        uint32_t off = s.best_tick - s.base_tick;
        if(off < BookSide::SPAN/8 || off >= BookSide::SPAN - BookSide::SPAN/8){
            uint32_t new_base = (s.best_tick > BookSide::SPAN/2) ? s.best_tick - BookSide::SPAN/2 : 0;
            if(new_base != s.base_tick) reanchor(s, is_bid, s.best_tick);
        }
    }
}

void OrderBook::side_add(BookSide& s, bool is_bid, uint32_t price, uint32_t shares){
    if(price % 100 == 0){
        uint32_t tick = price / 100;
        if(!s.anchored()){
            s.ladder.assign(BookSide::SPAN, 0);
            s.base_tick = (tick > BookSide::SPAN/2) ? tick - BookSide::SPAN/2 : 0;
        }
        if(s.in_window(tick)){
            uint32_t& slot = s.ladder[tick - s.base_tick];
            if(slot == 0) s.nonzero++;
            slot += shares;
            if(!s.best_valid || (is_bid ? tick > s.best_tick : tick < s.best_tick)){
                s.best_tick = tick; s.best_valid = true;
                maybe_reanchor(s, is_bid);
            }
            return;
        }
    }
    fallback_ops++;
    fallback_insert(s.fallback, is_bid, price, shares);
    maybe_reanchor(s, is_bid);
}

void OrderBook::side_remove(BookSide& s, bool is_bid, uint32_t price, uint32_t shares){
    if(price % 100 == 0 && s.anchored()){
        uint32_t tick = price / 100;
        if(s.in_window(tick)){
            uint32_t& slot = s.ladder[tick - s.base_tick];
            assert(shares <= slot && "ladder: level < shares (book desync)");
            uint32_t r = std::min(shares, slot);
            slot -= r;
            if(slot == 0){
                s.nonzero--;
                if(s.best_valid && tick == s.best_tick) rescan_best(s, is_bid);
            }
            return;
        }
    }
    fallback_ops++;
    fallback_remove(s.fallback, is_bid, price, shares);
}

void OrderBook::rescan_best(BookSide& s, bool is_bid){
    if(s.nonzero == 0){ s.best_valid = false; return; }
    if(is_bid){
        for(uint32_t i = s.best_tick - s.base_tick; i-- > 0; )
            if(s.ladder[i]){ s.best_tick = s.base_tick + i; return; }
    } else {
        for(uint32_t i = s.best_tick - s.base_tick + 1; i < BookSide::SPAN; i++)
            if(s.ladder[i]){ s.best_tick = s.base_tick + i; return; }
    }
    s.best_valid = false;
}

void OrderBook::add(uint32_t price, uint32_t shares, char side){
    side_add(side == 'B' ? bid : ask, side == 'B', price, shares);
    update_ofi();
}

void OrderBook::remove(uint32_t price, uint32_t shares, char side){
    side_remove(side == 'B' ? bid : ask, side == 'B', price, shares);
    update_ofi();
}

PriceLevel OrderBook::best_bid() const{
    PriceLevel w{0,0}, f{0,0};
    if(bid.best_valid) w = { bid.best_tick * 100, bid.ladder[bid.best_tick - bid.base_tick] };
    if(!bid.fallback.empty()) f = bid.fallback[0];
    if(w.shares == 0) return f;
    if(f.shares == 0) return w;
    return (w.price >= f.price) ? w : f;
}

PriceLevel OrderBook::best_ask() const{
    PriceLevel w{0,0}, f{0,0};
    if(ask.best_valid) w = { ask.best_tick * 100, ask.ladder[ask.best_tick - ask.base_tick] };
    if(!ask.fallback.empty()) f = ask.fallback[0];
    if(w.shares == 0) return f;
    if(f.shares == 0) return w;
    return (w.price <= f.price) ? w : f;
}

void OrderBook::update_ofi(){
    PriceLevel bb = best_bid();
    PriceLevel ba = best_ask();
    uint32_t current_bb_price = bb.shares ? bb.price : 0;
    uint32_t current_bb_shares = bb.shares;
    uint32_t current_ba_price = ba.shares ? ba.price : 0;
    uint32_t current_ba_shares = ba.shares;

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

void OrderBook::collect(const BookSide& s, bool is_bid, size_t max_levels,
                        std::vector<PriceLevel>& out) const{
    size_t fi = 0;
    if(s.anchored() && s.nonzero > 0){
        if(is_bid){
            uint32_t start = s.best_valid ? (s.best_tick - s.base_tick) : 0;
            for(uint32_t i = start + 1; i-- > 0 && out.size() < max_levels; ){
                if(!s.ladder[i]) continue;
                uint32_t p = (s.base_tick + i) * 100;
                while(fi < s.fallback.size() && s.fallback[fi].price > p && out.size() < max_levels)
                    out.push_back(s.fallback[fi++]);
                if(out.size() >= max_levels) break;
                out.push_back({p, s.ladder[i]});
            }
        } else {
            uint32_t start = s.best_valid ? (s.best_tick - s.base_tick) : 0;
            for(uint32_t i = start; i < BookSide::SPAN && out.size() < max_levels; i++){
                if(!s.ladder[i]) continue;
                uint32_t p = (s.base_tick + i) * 100;
                while(fi < s.fallback.size() && s.fallback[fi].price < p && out.size() < max_levels)
                    out.push_back(s.fallback[fi++]);
                if(out.size() >= max_levels) break;
                out.push_back({p, s.ladder[i]});
            }
        }
    }
    while(fi < s.fallback.size() && out.size() < max_levels)
        out.push_back(s.fallback[fi++]);
}

std::vector<PriceLevel> OrderBook::all_bids() const{
    std::vector<PriceLevel> v; collect(bid, true,  (size_t)-1, v); return v;
}
std::vector<PriceLevel> OrderBook::all_asks() const{
    std::vector<PriceLevel> v; collect(ask, false, (size_t)-1, v); return v;
}

void OrderBook::fill_snapshot(int levels, const std::string& symbol, uint64_t timestamp_ns, BookSnapshot& snap){
    snap.timestamp_ns = timestamp_ns;
    std::memset(snap.symbol, ' ', 8);
    std::memcpy(snap.symbol, symbol.c_str(), std::min((size_t) 8, symbol.size()));

    snap.ofi = (int32_t)ofi_accumulator;
    ofi_accumulator = 0;

    std::vector<PriceLevel> tmp;
    tmp.reserve(levels);
    collect(ask, false, levels, tmp);
    snap.num_asks = (uint8_t)tmp.size();
    for(size_t i = 0; i < tmp.size(); i++) snap.asks[i] = tmp[i];

    tmp.clear();
    collect(bid, true, levels, tmp);
    snap.num_bids = (uint8_t)tmp.size();
    for(size_t i = 0; i < tmp.size(); i++) snap.bids[i] = tmp[i];
}