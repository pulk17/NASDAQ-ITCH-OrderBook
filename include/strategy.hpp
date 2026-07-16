#pragma once
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>
#include <array>
#include <memory>
#include <algorithm>
#include "orderbook.hpp"

struct Strategy {
    // ts: exchange timestamp of the message that caused the update (ns since midnight)
    virtual void on_book_update(uint16_t locate, const OrderBook& book, uint64_t ts) = 0;
    virtual void finish() = 0;
    virtual ~Strategy() = default;
};

// Cross-sectional OFI study: runs the predictivity test + naive directional
// signal backtest (hold sign(last interval's OFI), 1 share, marked mid-to-mid,
// no costs) on EVERY symbol simultaneously. Scalar metrics stream for all
// 65,536 locates (~15MB state); full chart series are recorded only for
// locates in the detail set to keep memory and JSON bounded.
struct MultiOFIStudy : Strategy {
    struct Corr { double sx=0,sy=0,sxx=0,syy=0,sxy=0; uint64_t n=0;
        void add(double x,double y){sx+=x;sy+=y;sxx+=x*x;syy+=y*y;sxy+=x*y;n++;}
        double r() const { if(!n) return 0;
            double cov=sxy-sx*sy/n, vx=sxx-sx*sx/n, vy=syy-sy*sy/n;
            return (vx>0&&vy>0)?cov/std::sqrt(vx*vy):0; }
    };
    struct State {
        uint32_t counter=0;
        bool have_prev=false, have_last=false, have_pend=false;
        int64_t prev_acc=0; double prev_mid=0; int64_t last_ofi=0; int last_pos=0;
        int64_t pend_ofi=0; uint64_t pend_ts=0;   // signal awaiting its latency delay
        uint64_t con_hit=0, con_tot=0, pre_hit=0, pre_tot=0;
        Corr con_corr, pre_corr;
        double pnl=0, peak=0, maxdd=0;
        double cost=0, net=0, net_peak=0, net_maxdd=0; uint64_t shares_traded=0;
        double pnl_ret=0, cost_ret=0;          // return-normalized (unit: fraction of mid)
        double last_mid=0;
        double win_sum=0, loss_sum=0;                     // profit-factor components
        double ret_sum=0, ret_sum2=0, dn_sum2=0; uint64_t ret_n=0;   // t-stat / Sharpe / Sortino
        uint64_t wins=0, losses=0, flips=0, in_pos=0;
        std::vector<float>* eq=nullptr; std::vector<float>* net_eq=nullptr;
        std::vector<float>* mid_s=nullptr; std::vector<int32_t>* ofi_s=nullptr;
    };

    uint32_t interval;
    uint64_t latency_ns = 0;                  // signal -> execution delay, market time
    uint64_t ts_min = 0, ts_max = UINT64_MAX; // evaluation window (walk-forward splits)
    uint64_t crossed_samples = 0;             // guard: locked/crossed book at sample time
    std::unique_ptr<State[]> st;                          // indexed by locate
    struct Series { std::vector<float> eq, net_eq, mid; std::vector<int32_t> ofi; };
    std::vector<std::unique_ptr<Series>> detail_series;

    explicit MultiOFIStudy(uint32_t iv) : interval(iv), st(std::make_unique<State[]>(65536)) {}

    void enable_detail(uint16_t locate){
        if(st[locate].eq) return;
        detail_series.push_back(std::make_unique<Series>());
        Series& s = *detail_series.back();
        st[locate].eq = &s.eq; st[locate].net_eq = &s.net_eq;
        st[locate].mid_s = &s.mid; st[locate].ofi_s = &s.ofi;
    }

    void on_book_update(uint16_t locate, const OrderBook& book, uint64_t ts) override {
        if(ts < ts_min || ts >= ts_max) return;
        State& s = st[locate];
        if(++s.counter < interval) return;
        s.counter = 0;

        PriceLevel bb = book.best_bid(), ba = book.best_ask();
        if(bb.shares == 0 || ba.shares == 0) return;
        if(bb.price >= ba.price) crossed_samples++;
        double mid = (bb.price + ba.price) / 2.0 / 10000.0;
        int64_t acc = book.ofi_accumulator;

        if(s.have_prev){
            int64_t ofi = acc - s.prev_acc;
            double  ret = mid - s.prev_mid;

            // Latency model: a signal generated at time T becomes the acted
            // position only at the first sample >= T + latency_ns. A newer
            // signal arriving first supersedes it (cancel-replace). With
            // latency_ns == 0 this reduces exactly to the old 1-sample lag.
            if(s.have_pend && ts >= s.pend_ts + latency_ns){
                s.last_ofi = s.pend_ofi; s.have_last = true; s.have_pend = false;
            }

            if(ofi!=0 && ret!=0){ if((ofi>0)==(ret>0)) s.con_hit++; s.con_tot++; s.con_corr.add((double)ofi,ret); }
            if(s.have_last){
                int pos = (s.last_ofi>0)-(s.last_ofi<0);
                // Cost model: any position change trades |delta| shares at THIS
                // moment's quoted half-spread (market order crossing to touch).
                int delta = pos - s.last_pos;
                if(delta != 0){
                    // signed math: uint32 subtraction UNDERFLOWS on a crossed book
                    // (bid > ask happens pre-open / in halts); clamp cost at zero.
                    double half_spread = ((double)ba.price - (double)bb.price) / 2.0 / 10000.0;
                    if(half_spread < 0) half_spread = 0;
                    s.cost += std::abs(delta) * half_spread;
                    s.cost_ret += std::abs(delta) * half_spread / mid;
                    s.shares_traded += std::abs(delta);
                }
                if(pos!=0){ s.in_pos++; if(pos!=s.last_pos && s.last_pos!=0) s.flips++; }
                s.last_pos = pos;
                double step = pos*ret;
                s.pnl += step;
                if(s.prev_mid > 0) s.pnl_ret += pos*ret/s.prev_mid;
                if(s.pnl>s.peak) s.peak=s.pnl;
                if(s.peak-s.pnl>s.maxdd) s.maxdd=s.peak-s.pnl;
                s.net = s.pnl - s.cost;
                if(s.net>s.net_peak) s.net_peak=s.net;
                if(s.net_peak-s.net>s.net_maxdd) s.net_maxdd=s.net_peak-s.net;
                if(step>0){ s.wins++; s.win_sum+=step; } else if(step<0){ s.losses++; s.loss_sum-=step; }
                s.ret_sum+=step; s.ret_sum2+=step*step; if(step<0) s.dn_sum2+=step*step; s.ret_n++;
                if(s.last_ofi!=0 && ret!=0){ if((s.last_ofi>0)==(ret>0)) s.pre_hit++; s.pre_tot++; s.pre_corr.add((double)s.last_ofi,ret); }
            }
            s.last_mid = mid;
            if(s.eq){ s.eq->push_back((float)s.pnl); s.net_eq->push_back((float)s.net);
                      s.mid_s->push_back((float)mid); s.ofi_s->push_back((int32_t)ofi); }
            s.pend_ofi = ofi; s.pend_ts = ts; s.have_pend = true;
        }
        s.prev_acc = acc; s.prev_mid = mid; s.have_prev = true;
    }

    static double tstat(const State& s){
        if(s.ret_n < 2) return 0;
        double m = s.ret_sum/s.ret_n, v = s.ret_sum2/s.ret_n - m*m;
        return v>0 ? m/std::sqrt(v/s.ret_n) : 0;
    }
    // Per-sample ratios (annualization-agnostic — the sample is one interval).
    static double sharpe(const State& s){
        if(s.ret_n < 2) return 0;
        double m = s.ret_sum/s.ret_n, v = s.ret_sum2/s.ret_n - m*m;
        return v>0 ? m/std::sqrt(v) : 0;
    }
    static double sortino(const State& s){
        if(s.ret_n < 2) return 0;
        double m = s.ret_sum/s.ret_n, dd = s.dn_sum2/s.ret_n;
        return dd>0 ? m/std::sqrt(dd) : 0;
    }
    static double calmar(const State& s){        // net return over net max drawdown
        return s.net_maxdd>1e-9 ? s.net/s.net_maxdd : 0;
    }

    // Cross-symbol aggregate over locates with enough predictive samples.
    // *_bps figures are per-symbol MEDIANS of return-normalized P&L: means are
    // destroyed by thin tickers whose crossed/garbage quotes fake huge returns.
    struct Pooled { uint64_t syms=0, hit=0, tot=0; double pnl=0, cost=0, net=0, pnl_bps=0, net_bps=0;
                    double hit_pct() const { return tot ? 100.0*hit/tot : 0; } };
    Pooled pooled(uint64_t min_samples = 30) const {
        Pooled p;
        std::vector<double> pb, nb;
        for(uint32_t l=0; l<65536; l++){ const State& s=st[l];
            if(s.pre_tot < min_samples) continue;
            p.syms++; p.hit+=s.pre_hit; p.tot+=s.pre_tot;
            p.pnl+=s.pnl; p.cost+=s.cost; p.net+=s.net;
            pb.push_back(1e4*s.pnl_ret); nb.push_back(1e4*(s.pnl_ret - s.cost_ret)); }
        auto med = [](std::vector<double>& v){ if(v.empty()) return 0.0;
            size_t m = v.size()/2; std::nth_element(v.begin(), v.begin()+m, v.end()); return v[m]; };
        p.pnl_bps = med(pb); p.net_bps = med(nb);
        return p;
    }

    void finish() override {
        Pooled p = pooled();
        printf("\n=== cross-sectional OFI study (every %u updates, latency %.3f ms) ===\n",
               interval, latency_ns/1e6);
        printf("  symbols with >=30 predictive samples: %lu\n", p.syms);
        if(p.tot) printf("  pooled predictive hit-rate: %.2f%% over %lu samples\n", p.hit_pct(), p.tot);
        printf("  summed gross signal P&L: $%.2f/share-per-symbol (mid-to-mid, no costs)\n", p.pnl);
        printf("  summed NET P&L after spread costs: $%.2f (market orders at quoted half-spread)\n", p.net);
    }

    void write_json(const std::string& path, const std::array<std::string,65536>& names,
                    const std::string& file, uint64_t messages, const std::string& fingerprint,
                    uint64_t min_samples = 30, const std::string& extra_json = ""){
        FILE* f = fopen(path.c_str(), "w");
        if(!f){ perror("write_json"); return; }
        fprintf(f, "{\n\"interval\":%u,\"file\":\"%s\",\"messages\":%lu,\"fingerprint\":\"%s\",\n",
                interval, file.c_str(), messages, fingerprint.c_str());
        if(!extra_json.empty()) fprintf(f, "%s,\n", extra_json.c_str());

        std::vector<uint32_t> rows;
        for(uint32_t l=0; l<65536; l++) if(st[l].pre_tot>=min_samples && !names[l].empty()) rows.push_back(l);
        std::sort(rows.begin(), rows.end(), [&](uint32_t a, uint32_t b){ return st[a].pnl > st[b].pnl; });
        fprintf(f, "\"table\":[");
        for(size_t k=0; k<rows.size(); k++){
            const State& s = st[rows[k]];
            double wr = (s.wins+s.losses)? 100.0*s.wins/(s.wins+s.losses) : 0;
            double pf = s.loss_sum>0 ? s.win_sum/s.loss_sum : 0;
            fprintf(f, "%s{\"sym\":\"%s\",\"n\":%lu,\"hit\":%.2f,\"corr\":%.3f,\"pnl\":%.2f,\"cost\":%.2f,\"net\":%.2f,\"dd\":%.2f,"
                       "\"wr\":%.1f,\"pf\":%.2f,\"t\":%.1f,\"flips\":%lu,\"px\":%.2f,\"pnlb\":%.1f,\"netb\":%.1f,\"det\":%d}",
                    k?",\n":"\n", names[rows[k]].c_str(), s.pre_tot,
                    s.pre_tot?100.0*s.pre_hit/s.pre_tot:0, s.pre_corr.r(), s.pnl, s.cost, s.net, s.maxdd,
                    wr, pf, tstat(s), s.flips, s.last_mid, 1e4*s.pnl_ret, 1e4*(s.pnl_ret - s.cost_ret), s.eq?1:0);
        }
        fprintf(f, "],\n");

        fprintf(f, "\"detail\":{");
        bool first=true; char buf[32];
        for(uint32_t l=0; l<65536; l++){
            const State& s = st[l];
            if(!s.eq || s.eq->empty()) continue;
            size_t n=s.eq->size(), stride = n>2000 ? (n+1999)/2000 : 1;
            auto arr=[&](const char* nm, auto&& get){ fprintf(f,"\"%s\":[",nm);
                for(size_t i=0,k=0;i<n;i+=stride,k++) fprintf(f,"%s%s",k?",":"",get(i));
                fprintf(f,"]"); };
            fprintf(f, "%s\"%s\":{", first?"":",\n", names[l].c_str()); first=false;
            fprintf(f, "\"con_hit\":%.2f,\"pre_hit\":%.2f,\"pre_corr\":%.3f,\"pre_n\":%lu,\"con_n\":%lu,",
                    s.con_tot?100.0*s.con_hit/s.con_tot:0, s.pre_tot?100.0*s.pre_hit/s.pre_tot:0,
                    s.pre_corr.r(), s.pre_tot, s.con_tot);
            double wr=(s.wins+s.losses)?100.0*s.wins/(s.wins+s.losses):0;
            fprintf(f, "\"net\":%.4f,\"net_dd\":%.4f,\"cost\":%.4f,\"trades\":%lu,",
                    s.net, s.net_maxdd, s.cost, s.shares_traded);
            fprintf(f, "\"pnl\":%.4f,\"dd\":%.4f,\"wr\":%.1f,\"pf\":%.2f,\"t\":%.1f,\"flips\":%lu,"
                       "\"exposure\":%.1f,\"avg_win\":%.5f,\"avg_loss\":%.5f,"
                       "\"sharpe\":%.3f,\"sortino\":%.3f,\"calmar\":%.3f,",
                    s.pnl, s.maxdd, wr, s.loss_sum>0?s.win_sum/s.loss_sum:0, tstat(s), s.flips,
                    s.ret_n?100.0*s.in_pos/s.ret_n:0, s.wins?s.win_sum/s.wins:0, s.losses?s.loss_sum/s.losses:0,
                    sharpe(s), sortino(s), calmar(s));
            arr("equity",[&](size_t i){ snprintf(buf,32,"%.4f",(*s.eq)[i]); return buf; }); fprintf(f,",");
            arr("net_equity",[&](size_t i){ snprintf(buf,32,"%.4f",(*s.net_eq)[i]); return buf; }); fprintf(f,",");
            arr("mid",   [&](size_t i){ snprintf(buf,32,"%.4f",(*s.mid_s)[i]); return buf; }); fprintf(f,",");
            arr("ofi",   [&](size_t i){ snprintf(buf,32,"%d",(*s.ofi_s)[i]); return buf; });
            fprintf(f, "}");
        }
        fprintf(f, "}\n}\n");
        fclose(f);
        printf("  wrote %s (%zu table rows, %zu detail symbols)\n", path.c_str(), rows.size(), detail_series.size());
    }
};