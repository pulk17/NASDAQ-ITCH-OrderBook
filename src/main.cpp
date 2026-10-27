// PitchFork: replay a NASDAQ TotalView-ITCH 5.0 tape through the order-book
// engine. One pass over the tape; each mode below hooks into that pass.
//   (default)   throughput + book fingerprint
//   --bench     per-message dispatch latency histogram (rdtsc)
//   --study     cross-sectional OFI study -> results JSON   (study_runner.hpp)
//   --extract   per-symbol Strategy Lab features            (extractor.hpp)
//   --live      paced UDP stream of one symbol's book       (live_stream.hpp)
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bench.hpp"
#include "engine.hpp"
#include "extractor.hpp"
#include "io_uring_source.hpp"
#include "live_stream.hpp"
#include "messages.hpp"
#include "source.hpp"
#include "study_runner.hpp"

struct Options {
    const char* itch_path = nullptr;
    bool        bench = false;
    bool        io_uring = false;
    size_t      chunk = 0;                    // 0 = mmap the whole file
    std::string study;                        // SYM | all
    std::string detail;                       // SYM,SYM,… to chart in the study output
    std::string out;                          // study results JSON
    uint32_t    interval = 50;                // messages per sample (study + extract)
    uint64_t    min_samples = 30;
    std::string live;                         // symbol to stream
    double      speed = 240;                  // live market-time multiplier, 0 = unpaced
    std::string extract;                      // SYM,SYM,… | all
    std::string features_out = "report/features";
};

static Options parse_args(int argc, char** argv){
    Options o;
    o.itch_path = argv[1];
    for(int i = 2; i < argc; i++){
        std::string a = argv[i];
        bool has_value = i + 1 < argc;
        if(a == "--bench")                           o.bench = true;
        else if(a == "--io_uring")                   o.io_uring = true;
        else if(a == "--chunk" && has_value)         o.chunk = std::stoull(argv[++i]);
        else if(a == "--study" && has_value)         o.study = argv[++i];
        else if(a == "--detail" && has_value)        o.detail = argv[++i];
        else if(a == "--out" && has_value)           o.out = argv[++i];
        else if(a == "--interval" && has_value)      o.interval = std::stoul(argv[++i]);
        else if(a == "--min-samples" && has_value)   o.min_samples = std::stoull(argv[++i]);
        else if(a == "--live")                       o.live = (has_value && argv[i+1][0] != '-') ? argv[++i] : "AAPL";
        else if(a == "--speed" && has_value)         o.speed = std::stod(argv[++i]);
        else if(a == "--extract" && has_value)       o.extract = argv[++i];
        else if(a == "--features-out" && has_value)  o.features_out = argv[++i];
        else std::cerr << "Warning: ignoring unknown argument '" << a << "'\n";
    }
    return o;
}

static void pin_thread_to_core(int core_id){
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if(pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
        std::cerr << "Warning: failed to pin thread to core " << core_id << "\n";
}

int main(int argc, char** argv){
    if(argc < 2){
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file> [--bench] [--io_uring] [--chunk N]\n"
                     "         [--study SYM|all] [--detail SYM,…] [--out FILE] [--interval N] [--min-samples N]\n"
                     "         [--live SYM] [--speed N] [--extract SYM,…|all] [--features-out DIR]\n";
        return 1;
    }
    Options opt = parse_args(argc, argv);
    pin_thread_to_core(1);

    int fd = open(opt.itch_path, O_RDONLY);
    if(fd < 0){ std::cerr << "Failed to open '" << opt.itch_path << "': " << strerror(errno) << "\n"; return 1; }
    struct stat st;
    if(fstat(fd, &st) < 0){ std::cerr << "fstat failed: " << strerror(errno) << "\n"; close(fd); return 1; }
    size_t file_size = st.st_size;
    if(file_size == 0){ std::cerr << "'" << opt.itch_path << "' is empty\n"; close(fd); return 1; }
    const uint8_t* data = (const uint8_t*) mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(data == MAP_FAILED){ std::cerr << "mmap failed: " << strerror(errno) << "\n"; close(fd); return 1; }
    madvise((void*)data, file_size, MADV_SEQUENTIAL);

    std::optional<StudyRunner>      study;
    std::optional<FeatureExtractor> extract;
    std::optional<LiveStream>       live;
    if(!opt.study.empty())   study.emplace(opt.interval, opt.study, opt.detail);
    if(!opt.extract.empty()) extract.emplace(opt.extract, opt.interval);
    bool live_mode = !opt.live.empty() && !study;          // the study runs unpaced

    Engine engine;
    engine.track_ofi = study || extract || live_mode;

    LatencyBench bench;
    constexpr uint64_t BENCH_WARMUP = 100000;
    if(opt.bench) bench.calibrate();

    auto starttime = std::chrono::high_resolution_clock::now();   // setup above is not timed
    if(live_mode) live.emplace(opt.live, opt.speed, starttime);

    // The book is empty once the day ends, so hashing only the final state proves
    // nothing. Fold the full book in at every hour 10:00..16:00 instead: any
    // divergence between variants during the session changes the hash.
    uint64_t fingerprint = Engine::FNV_BASIS;
    uint64_t next_checkpoint = 36000000000000ULL;            // 10:00 ET, ns since midnight
    constexpr uint64_t LAST_CHECKPOINT = 57600000000000ULL;   // 16:00
    uint64_t message_count = 0;

    auto on_message = [&](const char* msg){
        uint64_t ts = itch_ts(msg);
        if(ts >= next_checkpoint && next_checkpoint <= LAST_CHECKPOINT){   // book as of just before this message
            fingerprint = engine.fingerprint(fingerprint);
            next_checkpoint += 3600000000000ULL;
        }

        uint64_t t0 = opt.bench ? rdtsc_start() : 0;
        uint16_t loc = engine.process_message(msg);
        if(opt.bench && message_count >= BENCH_WARMUP) bench.record(rdtsc_end() - t0);

        if(study){
            if(loc) study->on_update(engine, loc, ts);
            else if(msg[0] == 'R') study->on_directory(engine);
        }
        if(extract && loc) extract->on_update(engine, loc, ts);
        if(live) live->on_message(engine, ts, message_count);
        message_count++;
    };

    MmapSource    mmap_src(data, file_size);
    ChunkedSource chunk_src(data, file_size, opt.chunk ? opt.chunk : 1);
    IoUringSource iouring_src(fd, file_size);
    ByteSource* src = opt.io_uring ? (ByteSource*)&iouring_src
                    : opt.chunk    ? (ByteSource*)&chunk_src
                    :                (ByteSource*)&mmap_src;
    run_framed(*src, on_message);

    fingerprint = engine.fingerprint(fingerprint);   // final state (empty after a full day)
    double seconds = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - starttime).count();
    std::cout << "Book fingerprint: " << fingerprint << "\n"
              << "Total symbols: " << engine.symbol_to_locate.size() << "\n"
              << "Processed " << message_count << " messages in " << seconds << "s\n"
              << "Throughput: " << (message_count / seconds) / 1e6 << " M msg/sec \n"
              << "Ingestion:  " << (file_size / seconds) / 1e6 << " MB/sec\n";

    if(opt.bench) bench.report();
    if(study)   study->finish(engine, opt.itch_path, file_size, message_count, fingerprint, opt.min_samples, opt.out);
    if(extract) extract->write(opt.features_out, opt.itch_path, message_count, fingerprint, opt.min_samples);

    live.reset();                                    // stop the publisher before unmapping
    munmap((void*)data, file_size);
    close(fd);
    return 0;
}
