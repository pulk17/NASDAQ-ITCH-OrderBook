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
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file> [--bench] [--io_uring] [--study SYM] [--interval N] [--chunk N]\n";
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
    for(int i = 2; i < argc; i++){
        if(std::string(argv[i]) == "--bench") bench_mode = true;
        if(std::string(argv[i]) == "--chunk" && i + 1 < argc) chunk_size = std::stoull(argv[++i]);
        if(std::string(argv[i]) == "--io_uring") use_io_uring = true;
        if(std::string(argv[i]) == "--study" && i + 1 < argc) study_symbol = argv[++i];
        if(std::string(argv[i]) == "--interval" && i + 1 < argc) study_interval = std::stoul(argv[++i]);
        if(std::string(argv[i]) == "--out" && i + 1 < argc) out_path = argv[++i];
        if(std::string(argv[i]) == "--detail" && i + 1 < argc) detail_csv = argv[++i];
        if(std::string(argv[i]) == "--min-samples" && i + 1 < argc) min_samples = std::stoull(argv[++i]);
    }
    bool study_mode = !study_symbol.empty();

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

    Engine engine;

    LatencyBench bench;
    constexpr uint64_t BENCH_WARMUP = 100000;
    if(bench_mode) bench.calibrate();

    MultiOFIStudy study(study_interval);
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

    auto starttime = std::chrono::high_resolution_clock::now();
    uint64_t message_count = 0;
    uint16_t aapl_locate = 0;
    auto last_snapshot = starttime;

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

        if(study_mode && loc) study.on_book_update(loc, engine.books[loc]);

        if(study_mode && detail_resolved < detail_syms.size()){
            for(const auto& sym : detail_syms){
                auto it = engine.symbol_to_locate.find(sym);
                if(it != engine.symbol_to_locate.end() && !study.st[it->second].eq){
                    study.enable_detail(it->second);
                    detail_resolved++;
                }
            }
        }

        if(aapl_locate == 0){
            auto it = engine.symbol_to_locate.find("AAPL");
            if(it != engine.symbol_to_locate.end()) aapl_locate = it->second;
        }

        // live snapshot path -- disabled in study mode, which consumes the OFI accumulator
        if(!study_mode && message_count % 10000 == 0){
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_snapshot).count();
            if(elapsed >= 0.1 && aapl_locate){
                uint64_t ts = 0;
                for(int i = 0; i < 6; i++) ts = (ts << 8) | (uint8_t)buffer[5+i];
                BookSnapshot snap = {};
                engine.books[aapl_locate].fill_snapshot(5, "AAPL", ts, snap);
                snapshot_queue.push(snap);
                last_snapshot = now;
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
    std::cout << "AAPL locate: " << aapl_locate << "\n";

    auto endtime = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(endtime - starttime).count();
    std::cout << "Processed " << message_count << " messages in " << seconds << "s\n";
    std::cout << "Throughput: " << (message_count / seconds) / 1e6 << " M msg/sec \n";
    std::cout << "Ingestion:  " << (file_size / seconds) / 1e6 << " MB/sec\n";

    if(bench_mode)  bench.report();
    if(study_mode){
        study.finish();
        if(!out_path.empty())
            study.write_json(out_path, engine.locate_to_symbol, itch_path, message_count, std::to_string(fp_value), min_samples);
    }

    engine_running.store(false, std::memory_order_release);
    publisher_thread.join();

    munmap((void*)data, file_size);
    close(fd);
    close(sock);
    return 0;
}