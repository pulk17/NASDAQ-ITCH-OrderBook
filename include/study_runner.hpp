#pragma once
// --study: the cross-sectional OFI study plus its companion checks, all fed in
// the same pass over the tape.
//   - main study at the requested interval
//   - latency sweep: same signal, acted on only after a market-time delay
//   - walk-forward: interval tuned on the morning, judged on the afternoon
//   - guards: timestamp regressions, crossed books, too-good-to-be-true
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "engine.hpp"
#include "strategy.hpp"

struct StudyRunner {
    static constexpr uint64_t WF_SPLIT = 45900000000000ULL;   // 12:45 ET, ns since midnight

    MultiOFIStudy study;
    std::vector<uint64_t> sweep_lats = {100000, 1000000, 10000000, 100000000, 1000000000};
    std::vector<uint32_t> wf_ivals   = {25, 50, 100, 200};
    std::vector<std::unique_ptr<MultiOFIStudy>> lat_studies, wf_train, wf_test;
    std::vector<std::string> detail_syms;      // symbols that get full chart series
    size_t   detail_resolved = 0;
    uint64_t prev_ts = 0, ts_regressions = 0;

    // symbol: "all" or one ticker. detail_csv: tickers to chart (default: AAPL, or the symbol).
    StudyRunner(uint32_t interval, const std::string& symbol, const std::string& detail_csv)
        : study(interval) {
        for(uint64_t L : sweep_lats){
            lat_studies.push_back(std::make_unique<MultiOFIStudy>(interval));
            lat_studies.back()->latency_ns = L;
        }
        for(uint32_t iv : wf_ivals){
            wf_train.push_back(std::make_unique<MultiOFIStudy>(iv));
            wf_train.back()->ts_max = WF_SPLIT;
            wf_test.push_back(std::make_unique<MultiOFIStudy>(iv));
            wf_test.back()->ts_min = WF_SPLIT;
        }
        std::string list = detail_csv.empty() ? (symbol == "all" ? std::string("AAPL") : symbol) : detail_csv;
        size_t p = 0;
        while(p < list.size()){
            size_t c = list.find(',', p);
            if(c == std::string::npos) c = list.size();
            if(c > p) detail_syms.push_back(list.substr(p, c - p));
            p = c + 1;
        }
    }

    // Called after a stock-directory ('R') message: detail symbols become
    // resolvable as soon as their directory entry arrives.
    void on_directory(const Engine& engine){
        if(detail_resolved == detail_syms.size()) return;
        for(const auto& sym : detail_syms){
            auto it = engine.symbol_to_locate.find(sym);
            if(it != engine.symbol_to_locate.end() && !study.st[it->second].eq){
                study.enable_detail(it->second);
                detail_resolved++;
            }
        }
    }

    void on_update(const Engine& engine, uint16_t loc, uint64_t ts){
        if(ts < prev_ts) ts_regressions++;
        prev_ts = ts;
        const OrderBook& book = engine.books[loc];
        study.on_book_update(loc, book, ts);
        for(auto& s : lat_studies) s->on_book_update(loc, book, ts);
        for(auto& s : wf_train)    s->on_book_update(loc, book, ts);
        for(auto& s : wf_test)     s->on_book_update(loc, book, ts);
    }

    void finish(const Engine& engine, const char* itch_path, size_t file_size, uint64_t messages,
                uint64_t fingerprint, uint64_t min_samples, const std::string& out_path){
        study.finish();
        char buf[512];
        std::string extra;

        // latency sweep: how fast does the edge decay with reaction time?
        auto sweep_row = [&](const MultiOFIStudy& s, uint64_t lat){
            auto p = s.pooled(min_samples);
            snprintf(buf, sizeof buf,
                "{\"latency_ns\":%lu,\"hit\":%.2f,\"n\":%lu,\"pnl\":%.2f,\"net\":%.2f,\"pnl_bps\":%.2f,\"net_bps\":%.2f}",
                lat, p.hit_pct(), p.tot, p.pnl, p.net, p.pnl_bps, p.net_bps);
            return std::string(buf);
        };
        extra += "\"latency_sweep\":[" + sweep_row(study, 0);
        for(size_t i = 0; i < lat_studies.size(); i++)
            extra += "," + sweep_row(*lat_studies[i], sweep_lats[i]);
        extra += "],\n";

        // walk-forward: tune interval on the morning, judge it on the afternoon
        size_t best = 0;
        for(size_t i = 1; i < wf_train.size(); i++)
            if(wf_train[i]->pooled(min_samples).net_bps > wf_train[best]->pooled(min_samples).net_bps) best = i;
        extra += "\"walk_forward\":{\"split_ns\":" + std::to_string(WF_SPLIT) + ",\"intervals\":[";
        for(size_t i = 0; i < wf_ivals.size(); i++){
            auto a = wf_train[i]->pooled(min_samples), b = wf_test[i]->pooled(min_samples);
            snprintf(buf, sizeof buf,
                "%s{\"interval\":%u,\"train_net_bps\":%.2f,\"train_hit\":%.2f,\"test_net_bps\":%.2f,\"test_hit\":%.2f}",
                i?",":"", wf_ivals[i], a.net_bps, a.hit_pct(), b.net_bps, b.hit_pct());
            extra += buf;
        }
        snprintf(buf, sizeof buf, "],\"chosen_interval\":%u,\"chosen_test_net_bps\":%.2f},\n",
                 wf_ivals[best], wf_test[best]->pooled(min_samples).net_bps);
        extra += buf;

        // sanity guards: the liar detector
        auto pm = study.pooled(min_samples);
        snprintf(buf, sizeof buf,
            "\"guards\":{\"ts_regressions\":%lu,\"crossed_book_samples\":%lu,\"too_good_to_be_true\":%s},\n",
            ts_regressions, study.crossed_samples, pm.net_bps > 20.0 ? "true" : "false");
        extra += buf;

        // run manifest: everything needed to reproduce this exact result
#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif
        snprintf(buf, sizeof buf,
            "\"manifest\":{\"git_commit\":\"%s\",\"built\":\"%s %s\",\"data_bytes\":%zu,"
            "\"messages\":%lu,\"book_fingerprint\":\"%lu\",\"interval\":%u,"
            "\"cost_model\":\"quoted half-spread per share on every position change\"}",
            GIT_COMMIT, __DATE__, __TIME__, file_size, messages, fingerprint, study.interval);
        extra += buf;

        printf("  guards: ts_regressions=%lu crossed_samples=%lu%s\n",
               ts_regressions, study.crossed_samples,
               pm.net_bps > 20.0 ? "  [WARNING: net > 20 bps/symbol — too good, check for leakage]" : "");

        if(!out_path.empty())
            study.write_json(out_path, engine.locate_to_symbol, itch_path, messages,
                             std::to_string(fingerprint), min_samples, extra);
    }
};
