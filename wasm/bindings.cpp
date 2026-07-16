// C ABI surface for the WASM build. Plain extern "C" exports (no embind) keep
// the binary small; JS talks to it through the heap. The SAME engine.cpp /
// orderbook.cpp that hit 9.3M msg/s natively compile here unchanged -- the
// whole point of the I/O-free core split.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <emscripten/emscripten.h>
#include "engine.hpp"
#include "strategy.hpp"

static Engine* E = nullptr;
static MultiOFIStudy* S = nullptr;
static uint32_t study_interval = 50;
static uint64_t study_latency = 0;
static uint32_t locate_msgs[65536];   // per-symbol activity, drives the watchlist

// Persistent framer state: chunks arrive from JS at the caller's pace, and a
// message may straddle two chunks -- same carry logic as the native framer.
static uint8_t carry[128];
static size_t  carry_len = 0;
static uint64_t msg_count = 0;
static uint64_t last_ts = 0;   // ns since midnight, from the last processed message
static inline uint64_t ts48(const uint8_t* body){
    uint64_t t = 0; for(int i = 5; i < 11; i++) t = (t << 8) | body[i]; return t;
}

// Trade tape ring + session clock (fed from every processed message).
static double last_ts_ns = 0;
struct Trade { uint32_t locate, price, shares, aggr_buy, ts_ms; };
static Trade tape[256];
static uint64_t tape_n = 0;

static inline void observe(const char* buffer){
    uint64_t ts = 0;
    for(int i = 0; i < 6; i++) ts = (ts << 8) | (uint8_t)buffer[5+i];
    last_ts_ns = (double)ts;
    char t = buffer[0];
    if(t == 'E' || t == 'C'){
        uint64_t ref = __builtin_bswap64(*reinterpret_cast<const uint64_t*>(buffer + 11));
        uint32_t sh  = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(buffer + 19));
        auto it = E->orders.find(ref);
        if(it != E->orders.end()){
            const OrderInfo& o = it->second;
            Trade& tr = tape[tape_n % 256];
            tr.locate = o.locate; tr.price = o.price; tr.shares = sh;
            tr.aggr_buy = (o.side == 'S');            // resting sell hit => aggressive buyer
            tr.ts_ms = (uint32_t)(ts / 1000000ULL);
            tape_n++;
        }
    }
}

extern "C" {

static MultiOFIStudy* make_study(){
    auto* s = new MultiOFIStudy(study_interval);
    s->latency_ns = study_latency;
    return s;
}

EMSCRIPTEN_KEEPALIVE void engine_create(){
    delete E; E = new Engine(100000);           // browser-sized registry
    delete S; S = make_study();
    carry_len = 0; msg_count = 0; last_ts = 0; last_ts_ns = 0; tape_n = 0;
    memset(locate_msgs, 0, sizeof locate_msgs);
}

// Changing the interval or latency restarts the study (fresh stats) without
// resetting the book -- the live backtester's parameter knobs.
EMSCRIPTEN_KEEPALIVE void strategy_set_interval(int iv){
    study_interval = (uint32_t)iv;
    delete S; S = make_study();
}

EMSCRIPTEN_KEEPALIVE void strategy_set_latency(double ns){
    study_latency = (uint64_t)ns;
    delete S; S = make_study();
}

EMSCRIPTEN_KEEPALIVE void engine_feed(const uint8_t* chunk, int len_i){
    size_t len = (size_t)len_i;
    const uint8_t* p = chunk;
    const uint8_t* e = chunk + len;

    if(carry_len > 0){
        if(carry_len < 2){
            size_t take = std::min((size_t)(2 - carry_len), (size_t)(e - p));
            memcpy(carry + carry_len, p, take); carry_len += take; p += take;
        }
        if(carry_len >= 2){
            uint16_t length = __builtin_bswap16(*reinterpret_cast<const uint16_t*>(carry));
            size_t need = 2 + (size_t)length;
            if(length == 0 || need > sizeof(carry)) carry_len = 0;
            else {
                size_t take = std::min(need - carry_len, (size_t)(e - p));
                memcpy(carry + carry_len, p, take); carry_len += take; p += take;
                if(carry_len == need){
                    observe(reinterpret_cast<const char*>(carry + 2));
                    last_ts = ts48(carry + 2);
                    uint16_t loc = E->process_message(reinterpret_cast<const char*>(carry + 2));
                    if(loc){ locate_msgs[loc]++; S->on_book_update(loc, E->books[loc], last_ts); }
                    msg_count++; carry_len = 0;
                }
            }
        }
    }
    while(p + 2 <= e){
        uint16_t length = __builtin_bswap16(*reinterpret_cast<const uint16_t*>(p));
        if(length == 0){ p = e; break; }
        if(p + 2 + (size_t)length > e) break;
        observe(reinterpret_cast<const char*>(p + 2));
        last_ts = ts48(p + 2);
        uint16_t loc = E->process_message(reinterpret_cast<const char*>(p + 2));
        if(loc){ locate_msgs[loc]++; S->on_book_update(loc, E->books[loc], last_ts); }
        msg_count++;
        p += 2 + (size_t)length;
    }
    if(p < e){ carry_len = (size_t)(e - p); memcpy(carry, p, carry_len); }
}

// Feed whole messages from chunk until the tape clock passes target_ts (ns
// since midnight) or the window runs out of complete messages. Returns bytes
// consumed; the caller advances its cursor by that and re-hands the tail next
// call, so no carry buffer is needed. This is the live page's only feed path:
// message-granular pacing keeps 1x replay smooth (an 8KB block can straddle a
// full second of market time).
EMSCRIPTEN_KEEPALIVE int engine_feed_until(const uint8_t* chunk, int len_i, double target_ts){
    const uint8_t* p = chunk;
    const uint8_t* e = chunk + (size_t)len_i;
    while(p + 2 <= e){
        uint16_t length = __builtin_bswap16(*reinterpret_cast<const uint16_t*>(p));
        if(length == 0 || p + 2 + (size_t)length > e) break;
        uint64_t ts = ts48(p + 2);
        if((double)ts >= target_ts) break;
        observe(reinterpret_cast<const char*>(p + 2));
        last_ts = ts;
        uint16_t loc = E->process_message(reinterpret_cast<const char*>(p + 2));
        if(loc){ locate_msgs[loc]++; S->on_book_update(loc, E->books[loc], last_ts); }
        msg_count++;
        p += 2 + (size_t)length;
    }
    return (int)(p - chunk);
}

EMSCRIPTEN_KEEPALIVE double engine_msg_count(){ return (double)msg_count; }
EMSCRIPTEN_KEEPALIVE double engine_last_ts(){ return (double)last_ts; }
EMSCRIPTEN_KEEPALIVE int engine_symbol_count(){ return (int)E->symbol_to_locate.size(); }

EMSCRIPTEN_KEEPALIVE int engine_locate(const char* sym){
    auto it = E->symbol_to_locate.find(sym);
    return it == E->symbol_to_locate.end() ? 0 : (int)it->second;
}

// Write up to max_levels {price_units, shares} pairs into out (uint32 pairs),
// best-first. side: 0 = bids, 1 = asks. Returns levels written.
EMSCRIPTEN_KEEPALIVE int engine_levels(int locate, int side, uint32_t* out, int max_levels){
    auto v = side == 0 ? E->books[locate].all_bids() : E->books[locate].all_asks();
    int n = std::min((int)v.size(), max_levels);
    for(int i = 0; i < n; i++){ out[2*i] = v[i].price; out[2*i+1] = v[i].shares; }
    return n;
}

EMSCRIPTEN_KEEPALIVE double engine_ofi(int locate){
    return (double)E->books[locate].ofi_accumulator;
}

// Enumerate the symbol directory: writes the i-th known symbol into buf,
// returns its locate (0 when past the end).
EMSCRIPTEN_KEEPALIVE int engine_symbol_at(int idx, char* buf){
    int seen = 0;
    for(uint32_t l = 0; l < 65536; l++){
        if(E->locate_to_symbol[l].empty()) continue;
        if(seen++ == idx){
            snprintf(buf, 16, "%s", E->locate_to_symbol[l].c_str());
            return (int)l;
        }
    }
    return 0;
}

// Same, but only symbols that have actually traded/quoted in this tape --
// the directory holds ~8k names, the slice carries messages for a few dozen.
EMSCRIPTEN_KEEPALIVE int engine_active_at(int idx, char* buf){
    int seen = 0;
    for(uint32_t l = 1; l < 65536; l++){
        if(!locate_msgs[l] || E->locate_to_symbol[l].empty()) continue;
        if(seen++ == idx){
            snprintf(buf, 16, "%s", E->locate_to_symbol[l].c_str());
            return (int)l;
        }
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE double engine_locate_msgs(int locate){
    return (double)locate_msgs[locate];
}

// Live strategy stats for one symbol. field: 0 hit%, 1 samples, 2 gross $,
// 3 net $, 4 shares traded, 5 gross bps, 6 net bps, 7 flips, 8 last mid.
EMSCRIPTEN_KEEPALIVE double strategy_stat(int locate, int field){
    const auto& s = S->st[locate];
    switch(field){
        case 0: return s.pre_tot ? 100.0*s.pre_hit/s.pre_tot : 0;
        case 1: return (double)s.pre_tot;
        case 2: return s.pnl;
        case 3: return s.net;
        case 4: return (double)s.shares_traded;
        case 5: return 1e4*s.pnl_ret;
        case 6: return 1e4*(s.pnl_ret - s.cost_ret);
        case 7: return (double)s.flips;
        case 8: return s.last_mid;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE double trade_count(){ return (double)tape_n; }
// Absolute index i (from trade_count history, last 256 valid); writes 5 u32s.
EMSCRIPTEN_KEEPALIVE void trade_get(double i, uint32_t* out){
    const Trade& t = tape[(uint64_t)i % 256];
    out[0]=t.locate; out[1]=t.price; out[2]=t.shares; out[3]=t.aggr_buy; out[4]=t.ts_ms;
}

// Decimal fingerprint written into buf -- byte-identical book state proof
// against the native binary on the same input.
EMSCRIPTEN_KEEPALIVE void engine_fingerprint(char* buf){
    uint64_t fp = 1469598103934665603ULL;
    for(uint32_t l = 0; l < 65536; l++){
        for(const auto& lvl : E->books[l].all_bids()){
            fp ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; fp *= 1099511628211ULL;
        }
        for(const auto& lvl : E->books[l].all_asks()){
            fp ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; fp *= 1099511628211ULL;
        }
    }
    snprintf(buf, 32, "%llu", (unsigned long long)fp);
}

} // extern "C"