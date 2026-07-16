#include <iostream>
#include <cstdint>
#include <cstring>
#include <array>
#include <string> 
#include <chrono>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <fstream>
#include <unordered_set>
#include <vector>

#include "spsc_queue.hpp"
#include "messages.hpp"
#include "orderbook.hpp"
#include "engine.hpp"
#include "strategy.hpp"
#include "bench.hpp"
#include "source.hpp"
#include "io_uring_source.hpp"

inline void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_t current_thread = pthread_self();
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if(result != 0) std::cerr << "Warning: Failed to pin thread to core " << core_id << " (Do you have enough cores?)\n";
    else std::cout << "Successfully pinned thread " << current_thread << " to CPU core " << core_id << "\n";
}

int main(int argc, char** argv){

    if(argc < 2){
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file> [--bench] [--io_uring] [--study SYM]\n"
                     "         [--interval N] [--chunk N] [--live SYM] [--speed N]\n"
                     "         [--extract SYM,SYM,…|all] [--features-out DIR] [--min-samples N]\n";
        return 1;
    }
    const char* itch_path = argv[1];

    bool bench_mode = false;
    size_t chunk_size = 0;
    bool use_io_uring = false;
    std::string study_symbol;
    std::string detail_csv;
    uint64_t min_samples = 30;
    uint32_t study_interval = 50;
    std::string out_path;
    std::string live_symbol;               // --live [SYM]: stream one symbol's book over UDP
    double live_speed = 240;               // --speed N: market-time multiplier (0 = max, no pacing)
    std::string extract_list;              // --extract SYM,SYM,… | all: dump Strategy Lab features
    std::string features_out = "report/features";
    for(int i = 2; i < argc; i++){
        if(std::string(argv[i]) == "--bench") bench_mode = true;
        if(std::string(argv[i]) == "--chunk" && i + 1 < argc) chunk_size = std::stoull(argv[++i]);
        if(std::string(argv[i]) == "--io_uring") use_io_uring = true;
        if(std::string(argv[i]) == "--study" && i + 1 < argc) study_symbol = argv[++i];
        if(std::string(argv[i]) == "--interval" && i + 1 < argc) study_interval = std::stoul(argv[++i]);
        if(std::string(argv[i]) == "--out" && i + 1 < argc) out_path = argv[++i];
        if(std::string(argv[i]) == "--detail" && i + 1 < argc) detail_csv = argv[++i];
        if(std::string(argv[i]) == "--min-samples" && i + 1 < argc) min_samples = std::stoull(argv[++i]);
        if(std::string(argv[i]) == "--live"){ live_symbol = (i+1 < argc && argv[i+1][0] != '-') ? argv[++i] : "AAPL"; }
        if(std::string(argv[i]) == "--speed" && i + 1 < argc) live_speed = std::stod(argv[++i]);
        if(std::string(argv[i]) == "--extract" && i + 1 < argc) extract_list = argv[++i];
        if(std::string(argv[i]) == "--features-out" && i + 1 < argc) features_out = argv[++i];
    }
    bool study_mode = !study_symbol.empty();
    bool live_mode = !live_symbol.empty();
    bool extract_mode = !extract_list.empty();

    pin_thread_to_core(1);

    int fd = open(itch_path, O_RDONLY);
    if(fd < 0){ std::cerr << "Failed to open '" << itch_path << "': " << strerror(errno) << "\n"; return 1; }

    struct stat st;
    if(fstat(fd, &st) < 0){ std::cerr << "fstat failed: " << strerror(errno) << "\n"; close(fd); return 1; }

    size_t file_size = st.st_size;
    if(file_size == 0){ std::cerr << "'" << itch_path << "' is empty\n"; close(fd); return 1; }

    const uint8_t* data = (const uint8_t*) mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(data == MAP_FAILED){ std::cerr << "mmap failed: " << strerror(errno) << "\n"; close(fd); return 1; }
    madvise((void*)data, file_size, MADV_SEQUENTIAL);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0){ std::cerr << "UDP Socket creation failed: " << strerror(errno) << "\n"; return 1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Control channel: the relay forwards "subscribe SYM" from the browser here,
    // so the live symbol can be switched without restarting.
    int ctrl_sock = -1;
    if(live_mode){
        ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in caddr; memset(&caddr, 0, sizeof(caddr));
        caddr.sin_family = AF_INET; caddr.sin_port = htons(12346);
        caddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if(bind(ctrl_sock, (struct sockaddr*)&caddr, sizeof(caddr)) < 0){
            std::cerr << "Warning: control port 12346 busy — symbol switching disabled\n";
            close(ctrl_sock); ctrl_sock = -1;
        } else fcntl(ctrl_sock, F_SETFL, O_NONBLOCK);
    }

    Engine engine;

    LatencyBench bench;
    constexpr uint64_t BENCH_WARMUP = 100000;
    if(bench_mode) bench.calibrate();

    MultiOFIStudy study(study_interval);

    // Companion studies (near-free: they early-return on the sample counter).
    // 1) Latency sweep: same signal, executed only after a market-time delay.
    // 2) Walk-forward: interval tuned on the morning, judged on the afternoon.
    const uint64_t WF_SPLIT = 45900000000000ULL;   // 12:45 ET, ns since midnight
    std::vector<uint64_t> sweep_lats = {100000, 1000000, 10000000, 100000000, 1000000000};
    std::vector<uint32_t> wf_ivals   = {25, 50, 100, 200};
    std::vector<std::unique_ptr<MultiOFIStudy>> lat_studies, wf_train, wf_test;
    if(study_mode){
        for(uint64_t L : sweep_lats){
            lat_studies.push_back(std::make_unique<MultiOFIStudy>(study_interval));
            lat_studies.back()->latency_ns = L;
        }
        for(uint32_t iv : wf_ivals){
            wf_train.push_back(std::make_unique<MultiOFIStudy>(iv));
            wf_train.back()->ts_max = WF_SPLIT;
            wf_test.push_back(std::make_unique<MultiOFIStudy>(iv));
            wf_test.back()->ts_min = WF_SPLIT;
        }
    }
    uint64_t prev_ts = 0, ts_regressions = 0;      // guard: tape must be time-ordered

    std::vector<std::string> detail_syms;
    if(study_mode){
        std::string src_list = detail_csv.empty()
            ? (study_symbol == "all" ? std::string("AAPL") : study_symbol) : detail_csv;
        size_t p = 0;
        while(p < src_list.size()){
            size_t c = src_list.find(',', p);
            if(c == std::string::npos) c = src_list.size();
            if(c > p) detail_syms.push_back(src_list.substr(p, c - p));
            p = c + 1;
        }
    }
    size_t detail_resolved = 0;

    // --extract: sample Strategy Lab features straight off the original tape — no
    // slicing. Every `study_interval` messages for a wanted symbol we record the
    // same five features the browser's WASM path computes, so a lab run over these
    // files and a lab run over a WASM replay agree exactly.
    struct Feat { double t, mid, spread, ofi, imb; };
    std::unordered_set<std::string> extract_want;
    bool extract_all = (extract_list == "all");
    if(extract_mode && !extract_all){
        size_t p = 0;
        while(p < extract_list.size()){
            size_t c = extract_list.find(',', p);
            if(c == std::string::npos) c = extract_list.size();
            if(c > p) extract_want.insert(extract_list.substr(p, c - p));
            p = c + 1;
        }
    }
    // locate -> slot in ext_feat; -2 not yet resolved, -1 resolved as unwanted.
    // Memoising the verdict keeps the hot path an array read, not a hash lookup.
    std::vector<int32_t>  ext_slot(65536, -2);
    std::vector<uint32_t> ext_since(65536, 0);
    std::vector<std::vector<Feat>> ext_feat;
    std::vector<std::string> ext_sym;

    auto starttime = std::chrono::high_resolution_clock::now();
    uint64_t message_count = 0;
    auto last_snapshot = starttime;

    // Live-stream state: skip premarket fast, then pace from the open so the
    // book is watchable. A small stats frame carries throughput/progress.
    using clk = std::chrono::high_resolution_clock;
    #pragma pack(push,1)
    struct LiveStats { uint64_t ts_ns; uint64_t msgs; double mps; double progress; };
    #pragma pack(pop)
    const uint64_t LIVE_START_NS = 33900000000000ULL;   // 09:25
    const uint64_t LIVE_END_NS   = 57600000000000ULL;   // 16:00 close — stop before after-hours
    uint16_t live_locate = 0;
    bool live_started = false;
    uint64_t live_ts_start = 0;
    double slept_s = 0;                    // total pacing sleep, excluded from the reported rate
    auto live_wall_start = starttime, last_pub = starttime, last_stats = starttime;

    SPSCQueue<BookSnapshot, 1024> snapshot_queue;
    std::atomic<bool> engine_running{true};

    std::thread publisher_thread([&](){
        BookSnapshot local_snap;
        while(engine_running.load(std::memory_order_relaxed)){
            if(snapshot_queue.pop(local_snap)){
                ssize_t written = sendto(sock, &local_snap, sizeof(BookSnapshot), 0, (struct sockaddr*)&addr, sizeof(addr));
                if(written < 0) std::cerr << "UDP Send Failed\n";
            }
            else std::this_thread::yield();
        }
    });

    auto process_message = [&](const char* buffer){

        uint64_t t0 = 0;
        if(bench_mode) t0 = rdtsc_start();

        uint16_t loc = engine.process_message(buffer);

        if(bench_mode && message_count >= BENCH_WARMUP)
            bench.record(rdtsc_end() - t0);

        if(study_mode && loc){
            uint64_t ts = 0;
            for(int i = 0; i < 6; i++) ts = (ts << 8) | (uint8_t)buffer[5+i];
            if(ts < prev_ts) ts_regressions++;
            prev_ts = ts;
            study.on_book_update(loc, engine.books[loc], ts);
            for(auto& sw : lat_studies) sw->on_book_update(loc, engine.books[loc], ts);
            for(auto& sw : wf_train)    sw->on_book_update(loc, engine.books[loc], ts);
            for(auto& sw : wf_test)     sw->on_book_update(loc, engine.books[loc], ts);
        }

        if(study_mode && detail_resolved < detail_syms.size()){
            for(const auto& sym : detail_syms){
                auto it = engine.symbol_to_locate.find(sym);
                if(it != engine.symbol_to_locate.end() && !study.st[it->second].eq){
                    study.enable_detail(it->second);
                    detail_resolved++;
                }
            }
        }

        if(extract_mode && loc){
            int32_t slot = ext_slot[loc];
            if(slot == -2){
                const std::string& sym = engine.locate_to_symbol[loc];
                if(sym.empty()) slot = -2;                       // directory not seen yet — retry
                else if(extract_all || extract_want.count(sym)){
                    slot = (int32_t)ext_feat.size();
                    ext_feat.emplace_back(); ext_sym.push_back(sym);
                    ext_slot[loc] = slot;
                } else ext_slot[loc] = slot = -1;
            }
            if(slot >= 0 && ++ext_since[loc] >= study_interval){
                ext_since[loc] = 0;
                auto bids = engine.books[loc].all_bids();
                auto asks = engine.books[loc].all_asks();
                if(!bids.empty() && !asks.empty() && bids[0].price < asks[0].price){
                    // top-2 depth per side, matching engine_levels(…, max_levels=2)
                    double bsz = bids[0].shares + (bids.size() > 1 ? bids[1].shares : 0);
                    double asz = asks[0].shares + (asks.size() > 1 ? asks[1].shares : 0);
                    double bid = bids[0].price / 1e4, ask = asks[0].price / 1e4;
                    uint64_t ts = 0;
                    for(int i = 0; i < 6; i++) ts = (ts << 8) | (uint8_t)buffer[5+i];
                    ext_feat[slot].push_back({ double(ts) / 1e9, (bid + ask) / 2, ask - bid,
                                               (double)engine.books[loc].ofi_accumulator,
                                               (bsz + asz) ? (bsz - asz) / (bsz + asz) : 0.0 });
                }
            }
        }

        // Live stream: pace from the open through the close and publish the chosen
        // symbol's book (60/s) plus a throughput/progress frame (5/s). The engine
        // processes every symbol, so switching live_locate is instant. Study off.
        if(live_mode && !study_mode){
            if(live_locate == 0){
                auto it = engine.symbol_to_locate.find(live_symbol);
                if(it != engine.symbol_to_locate.end()) live_locate = it->second;
            }
            uint64_t ts = 0;
            for(int i = 0; i < 6; i++) ts = (ts << 8) | (uint8_t)buffer[5+i];
            // Only touch the wall clock every 256 messages: a clock read per message
            // would throttle the engine well below its true rate. 256 messages is a
            // sub-millisecond slice of market time, so pacing stays smooth.
            if(live_locate && ts >= LIVE_START_NS && ts < LIVE_END_NS && (message_count & 255) == 0){
                // control channel: browser asks (via the relay) to switch symbol
                if(ctrl_sock >= 0){
                    char req[16]; ssize_t r;
                    while((r = recvfrom(ctrl_sock, req, sizeof(req)-1, 0, nullptr, nullptr)) > 0){
                        req[r] = 0;
                        while(r > 0 && (req[r-1] == '\n' || req[r-1] == ' ')) req[--r] = 0;
                        auto it = engine.symbol_to_locate.find(req);
                        if(it != engine.symbol_to_locate.end()){ live_locate = it->second; live_symbol = req; }
                    }
                }
                auto now = clk::now();
                if(!live_started){ live_started = true; live_ts_start = ts;
                    live_wall_start = last_pub = last_stats = now; }
                if(live_speed > 0){
                    // pace by real wall time; track sleep separately so throughput
                    // can report the engine's compute rate, not the paced average
                    double target = double(ts - live_ts_start) / 1e9 / live_speed;
                    double actual = std::chrono::duration<double>(now - live_wall_start).count();
                    if(target > actual + 0.001){
                        auto s0 = clk::now();
                        std::this_thread::sleep_for(std::chrono::duration<double>(target - actual));
                        now = clk::now();
                        slept_s += std::chrono::duration<double>(now - s0).count();
                    }
                }
                if(std::chrono::duration<double>(now - last_pub).count() >= 0.016){
                    BookSnapshot snap = {};
                    engine.books[live_locate].fill_snapshot(5, live_symbol.c_str(), ts, snap);
                    snapshot_queue.push(snap);
                    last_pub = now;
                }
                if(std::chrono::duration<double>(now - last_stats).count() >= 0.2){
                    // report the engine's true compute rate: wall time minus pacing sleep
                    double compute_s = std::chrono::duration<double>(now - starttime).count() - slept_s;
                    double prog = std::min(1.0, std::max(0.0, (double(ts)/1e9 - 34200) / (57600 - 34200)));
                    LiveStats st{ ts, message_count, message_count / std::max(compute_s, 1e-9), prog };
                    sendto(sock, &st, sizeof st, 0, (struct sockaddr*)&addr, sizeof(addr));
                    last_stats = now;
                }
            }
        }

        message_count++;
    };

    MmapSource    mmap_src(data, file_size);
    ChunkedSource chunk_src(data, file_size, chunk_size ? chunk_size : 1);
    IoUringSource iouring_src(fd, file_size);
    ByteSource*   src;
    if(use_io_uring)    src = &iouring_src;
    else if(chunk_size) src = &chunk_src;
    else                src = &mmap_src;
    run_framed(*src, process_message);

    uint64_t fp_value = 0;
    {
        uint64_t fp = 1469598103934665603ULL;
        for(uint32_t l = 0; l < 65536; l++){
            for(const auto& lvl : engine.books[l].all_bids()){
                fp ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; fp *= 1099511628211ULL;
            }
            for(const auto& lvl : engine.books[l].all_asks()){
                fp ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; fp *= 1099511628211ULL;
            }
        }
        std::cout << "Book fingerprint: " << fp << "\n";
        fp_value = fp;
    }

    std::cout << "Total symbols: " << engine.symbol_to_locate.size() << "\n";

    auto endtime = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(endtime - starttime).count();
    std::cout << "Processed " << message_count << " messages in " << seconds << "s\n";
    std::cout << "Throughput: " << (message_count / seconds) / 1e6 << " M msg/sec \n";
    std::cout << "Ingestion:  " << (file_size / seconds) / 1e6 << " MB/sec\n";

    if(bench_mode)  bench.report();
    if(study_mode){
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
            GIT_COMMIT, __DATE__, __TIME__, file_size, message_count, fp_value, study_interval);
        extra += buf;

        printf("  guards: ts_regressions=%lu crossed_samples=%lu%s\n",
               ts_regressions, study.crossed_samples,
               pm.net_bps > 20.0 ? "  [WARNING: net > 20 bps/symbol — too good, check for leakage]" : "");

        if(!out_path.empty())
            study.write_json(out_path, engine.locate_to_symbol, itch_path, message_count,
                             std::to_string(fp_value), min_samples, extra);
    }

    if(extract_mode){
        mkdir(features_out.c_str(), 0755);
        std::string idx = "{\n \"tape\": \"" + std::string(itch_path) + "\",\n"
            " \"interval\": " + std::to_string(study_interval) + ",\n"
            " \"stride\": 5,\n"
            " \"fields\": [\"t\",\"mid\",\"spread\",\"ofi\",\"imb\"],\n"
            " \"t_unit\": \"seconds since midnight ET\",\n"
            " \"book_fingerprint\": \"" + std::to_string(fp_value) + "\",\n"
            " \"messages\": " + std::to_string(message_count) + ",\n"
            " \"symbols\": [";
        // Rank by sample count so the Lab's picker leads with the liquid names.
        std::vector<size_t> order(ext_feat.size());
        for(size_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b){ return ext_feat[a].size() > ext_feat[b].size(); });
        size_t written = 0, bytes = 0;
        for(size_t k = 0; k < order.size(); k++){
            size_t i = order[k];
            const auto& f = ext_feat[i];
            if(f.size() < min_samples) continue;                 // too thin to backtest
            std::string file = ext_sym[i] + ".f64";
            std::ofstream o(features_out + "/" + file, std::ios::binary);
            o.write(reinterpret_cast<const char*>(f.data()), f.size() * sizeof(Feat));
            char b[256];
            snprintf(b, sizeof b, "%s\n  {\"sym\":\"%s\",\"file\":\"%s\",\"samples\":%zu,\"bytes\":%zu,"
                     "\"from\":%.3f,\"to\":%.3f}",
                     written ? "," : "", ext_sym[i].c_str(), file.c_str(), f.size(),
                     f.size() * sizeof(Feat), f.front().t, f.back().t);
            idx += b;
            written++; bytes += f.size() * sizeof(Feat);
        }
        idx += "\n ]\n}\n";
        std::ofstream(features_out + "/index.json") << idx;
        std::cout << "Extracted " << written << " symbols (" << bytes / 1e6 << " MB) -> "
                  << features_out << "/\n";
    }

    engine_running.store(false, std::memory_order_release);
    publisher_thread.join();

    munmap((void*)data, file_size);
    close(fd);
    close(sock);
    if(ctrl_sock >= 0) close(ctrl_sock);
    return 0;
}