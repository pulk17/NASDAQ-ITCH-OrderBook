// Differential tests: the tick ladder + fallback + re-anchor book against a plain
// std::map reference, and the framer against every possible chunk split.
// Built twice by CMake: default SPAN, and PF_SPAN=8 (nearly everything goes
// through the fallback and re-anchors constantly).
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <map>
#include <random>
#include <vector>

#include "orderbook.hpp"
#include "source.hpp"

struct Live { uint32_t price, shares; char side; };

static std::vector<PriceLevel> levels(const std::map<uint32_t, uint64_t>& m, bool bid){
    std::vector<PriceLevel> v;
    for(auto& [p, q] : m) v.push_back({p, (uint32_t)q});
    if(bid) std::reverse(v.begin(), v.end());
    return v;
}

static bool same(const std::vector<PriceLevel>& a, const std::vector<PriceLevel>& b){
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); i++)
        if(a[i].price != b[i].price || a[i].shares != b[i].shares) return false;
    return true;
}

static void book_vs_reference(uint32_t seed, int ops){
    std::mt19937 rng(seed);
    OrderBook book;
    std::map<uint32_t, uint64_t> ref_bid, ref_ask;
    std::vector<Live> live;
    int64_t mid = 1'500'000;                          // $150.0000 in ITCH 1/10000ths

    for(int i = 0; i < ops; i++){
        if(rng() % 64 == 0) mid += (int64_t)(rng() % 4001) * 100 - 200'000;   // jumps force re-anchor
        if(mid < 10'000) mid = 10'000;
        if(live.empty() || rng() % 100 < 55){
            char side = (rng() & 1) ? 'B' : 'S';
            int64_t off = (int64_t)(rng() % 300) * 100;
            int64_t p = side == 'B' ? mid - off : mid + off;
            if(rng() % 20 == 0) p += rng() % 100;                          // sub-penny
            if(rng() % 50 == 0) p += (side == 'B' ? -1 : 1) * 5'000'000;   // far outside window
            if(p <= 0) p = 1;
            uint32_t q = 1 + rng() % 1000;
            book.add((uint32_t)p, q, side);
            (side == 'B' ? ref_bid : ref_ask)[(uint32_t)p] += q;
            live.push_back({(uint32_t)p, q, side});
        } else {
            size_t k = rng() % live.size();
            Live& o = live[k];
            uint32_t q = (rng() & 1) ? o.shares : 1 + rng() % o.shares;    // delete or partial
            book.remove(o.price, q, o.side);
            auto& ref = o.side == 'B' ? ref_bid : ref_ask;
            if((ref[o.price] -= q) == 0) ref.erase(o.price);
            if((o.shares -= q) == 0){ live[k] = live.back(); live.pop_back(); }
        }

        PriceLevel bb = book.best_bid(), ba = book.best_ask();
        assert(ref_bid.empty() ? bb.shares == 0 : (bb.price == ref_bid.rbegin()->first && bb.shares == ref_bid.rbegin()->second));
        assert(ref_ask.empty() ? ba.shares == 0 : (ba.price == ref_ask.begin()->first && ba.shares == ref_ask.begin()->second));
        if(i % 997 == 0){
            assert(same(book.all_bids(), levels(ref_bid, true)));
            assert(same(book.all_asks(), levels(ref_ask, false)));
        }
    }
    assert(same(book.all_bids(), levels(ref_bid, true)));
    assert(same(book.all_asks(), levels(ref_ask, false)));
    printf("book seed %u: %d ops, %lu re-anchors, %lu fallback ops — matches reference\n",
           seed, ops, book.reanchor_count, book.fallback_ops);
}

// Frames of random length, cut into chunks of every size from 1 byte up: each
// message must come out exactly once, in order, whatever the split.
static void framing(){
    std::mt19937 rng(7);
    std::vector<uint8_t> tape;
    std::vector<uint32_t> want;
    for(uint32_t id = 0; id < 2000; id++){
        uint16_t len = 4 + rng() % 60;
        tape.push_back(len >> 8); tape.push_back(len & 0xff);
        tape.push_back(id >> 8);  tape.push_back(id & 0xff);
        for(int j = 2; j < len; j++) tape.push_back(rng());
        want.push_back(id);
    }
    for(size_t chunk = 1; chunk <= 257; chunk++){
        ChunkedSource src(tape.data(), tape.size(), chunk);
        std::vector<uint32_t> got;
        run_framed(src, [&](const char* m){ got.push_back((uint8_t)m[0] << 8 | (uint8_t)m[1]); });
        assert(got == want);
    }
    printf("framing: 2000 messages identical across chunk sizes 1..257\n");
}

int main(){
    for(uint32_t seed = 1; seed <= 5; seed++) book_vs_reference(seed, 200'000);
    framing();
    printf("PASS (SPAN=%u)\n", BookSide::SPAN);
}
