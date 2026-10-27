#pragma once
// --extract: sample Strategy Lab features straight off the original tape, no
// slicing. Every `interval` messages for a wanted symbol, record the five
// features the browser's WASM path computes, then write one raw float64 file
// per symbol plus an activity-ranked index.json.
#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>

#include "engine.hpp"

struct FeatureExtractor {
    struct Feat { double t, mid, spread, ofi, imb; };

    uint32_t interval;
    bool all;
    std::unordered_set<std::string> want;
    // locate -> slot in feat; -2 not yet resolved, -1 resolved as unwanted.
    // Memoising the verdict keeps the hot path an array read, not a hash lookup.
    std::vector<int32_t>  slot_of = std::vector<int32_t>(65536, -2);
    std::vector<uint32_t> since   = std::vector<uint32_t>(65536, 0);
    std::vector<std::vector<Feat>> feat;
    std::vector<std::string> sym;

    // list: "all" or "SYM,SYM,…"
    FeatureExtractor(const std::string& list, uint32_t iv) : interval(iv), all(list == "all") {
        if(all) return;
        size_t p = 0;
        while(p < list.size()){
            size_t c = list.find(',', p);
            if(c == std::string::npos) c = list.size();
            if(c > p) want.insert(list.substr(p, c - p));
            p = c + 1;
        }
    }

    void on_update(const Engine& engine, uint16_t loc, uint64_t ts){
        int32_t slot = slot_of[loc];
        if(slot == -2){
            const std::string& s = engine.locate_to_symbol[loc];
            if(s.empty()) return;                                  // directory not seen yet, retry later
            if(all || want.count(s)){
                slot = (int32_t)feat.size();
                feat.emplace_back(); sym.push_back(s);
            } else slot = -1;
            slot_of[loc] = slot;
        }
        if(slot < 0 || ++since[loc] < interval) return;
        since[loc] = 0;

        const OrderBook& book = engine.books[loc];
        auto bids = book.all_bids();
        auto asks = book.all_asks();
        if(bids.empty() || asks.empty() || bids[0].price >= asks[0].price) return;
        // top-2 depth per side, matching engine_levels(…, max_levels=2)
        double bsz = bids[0].shares + (bids.size() > 1 ? bids[1].shares : 0);
        double asz = asks[0].shares + (asks.size() > 1 ? asks[1].shares : 0);
        double bid = bids[0].price / 1e4, ask = asks[0].price / 1e4;
        feat[slot].push_back({ double(ts) / 1e9, (bid + ask) / 2, ask - bid,
                               (double)book.ofi_accumulator,
                               (bsz + asz) ? (bsz - asz) / (bsz + asz) : 0.0 });
    }

    void write(const std::string& dir, const char* itch_path, uint64_t messages,
               uint64_t fingerprint, uint64_t min_samples) const {
        mkdir(dir.c_str(), 0755);
        std::string idx = "{\n \"tape\": \"" + std::string(itch_path) + "\",\n"
            " \"interval\": " + std::to_string(interval) + ",\n"
            " \"stride\": 5,\n"
            " \"fields\": [\"t\",\"mid\",\"spread\",\"ofi\",\"imb\"],\n"
            " \"t_unit\": \"seconds since midnight ET\",\n"
            " \"book_fingerprint\": \"" + std::to_string(fingerprint) + "\",\n"
            " \"messages\": " + std::to_string(messages) + ",\n"
            " \"symbols\": [";
        // Rank by sample count so the Lab's picker leads with the liquid names.
        std::vector<size_t> order(feat.size());
        for(size_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b){ return feat[a].size() > feat[b].size(); });
        size_t written = 0, bytes = 0;
        for(size_t i : order){
            const auto& f = feat[i];
            if(f.size() < min_samples) continue;                 // too thin to backtest
            std::string file = sym[i] + ".f64";
            std::ofstream(dir + "/" + file, std::ios::binary)
                .write(reinterpret_cast<const char*>(f.data()), f.size() * sizeof(Feat));
            char b[256];
            snprintf(b, sizeof b, "%s\n  {\"sym\":\"%s\",\"file\":\"%s\",\"samples\":%zu,\"bytes\":%zu,"
                     "\"from\":%.3f,\"to\":%.3f}",
                     written ? "," : "", sym[i].c_str(), file.c_str(), f.size(),
                     f.size() * sizeof(Feat), f.front().t, f.back().t);
            idx += b;
            written++; bytes += f.size() * sizeof(Feat);
        }
        idx += "\n ]\n}\n";
        std::ofstream(dir + "/index.json") << idx;
        std::cout << "Extracted " << written << " symbols (" << bytes / 1e6 << " MB) -> " << dir << "/\n";
    }
};
