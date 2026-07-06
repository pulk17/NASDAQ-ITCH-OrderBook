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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>

#include "spsc_queue.hpp"
#include "messages.hpp"
#include "orderbook.hpp"
#include "bench.hpp"
#include <ankerl/unordered_dense.h>

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
        std::cerr << "Usage: " << argv[0] << " <path-to-itch-file>\n";
        return 1;
    }
    const char* itch_path = argv[1];

    bool bench_mode = false;
    bool measure_window = false;
    for(int i = 2; i < argc; i++){
        if(std::string(argv[i]) == "--bench") bench_mode = true;
        if(std::string(argv[i]) == "--window-histo") measure_window = true;
    }

    pin_thread_to_core(1);

    int fd = open(itch_path, O_RDONLY);
    if(fd < 0){
        std::cerr << "Failed to open '" << itch_path << "': " << strerror(errno) << "\n";
        return 1;
    }

    struct stat st;
    if(fstat(fd, &st) < 0){
        std::cerr << "fstat failed on '" << itch_path << "': " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    size_t file_size = st.st_size;
    if(file_size == 0){
        std::cerr << "'" << itch_path << "' is empty\n";
        close(fd);
        return 1;
    }

    const uint8_t* data = (const uint8_t*) mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(data == MAP_FAILED){
        std::cerr << "mmap failed: " << strerror(errno) << "\n";
        close(fd);
        return 1;
    }

    const uint8_t* ptr = data;
    const uint8_t* end = data + file_size; 
    madvise((void*)data, file_size, MADV_SEQUENTIAL);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0) {
        std::cerr << "UDP Socket creation failed: " << strerror(errno) << "\n";
        return 1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    std::array<std::string, 65536> locate_to_symbol;
    ankerl::unordered_dense::map<std::string, uint16_t> symbol_to_locate;

    auto books = std::make_unique<OrderBook[]>(65536);

    ankerl::unordered_dense::map<uint64_t, OrderInfo> orders;
    orders.max_load_factor(0.5);
    orders.reserve(8000000);   

    LatencyBench bench;
    constexpr uint64_t BENCH_WARMUP = 100000;   // discard cold-cache/branch-predictor warmup
    if(bench_mode) bench.calibrate();

    constexpr uint32_t HISTMAX = 4096;                 // ticks; farther = "stub", lumped in overflow
    std::vector<uint64_t> tick_hist(HISTMAX, 0);
    uint64_t wh_total = 0, wh_overflow = 0, wh_subpenny = 0, wh_nomid = 0;

    auto record_add_distance = [&](const OrderBook& bk, uint32_t evt_price){
        if(bk.bids.empty() || bk.asks.empty()){ wh_nomid++; return; }  // no two-sided mid yet
        uint64_t mid  = ((uint64_t)bk.bids[0].price + bk.asks[0].price) / 2;
        uint64_t dist = (evt_price > mid) ? (evt_price - mid) : (mid - evt_price);
        uint64_t ticks = dist / 100;
        wh_total++;
        if(evt_price % 100 != 0) wh_subpenny++;
        if(ticks >= HISTMAX) wh_overflow++;
        else tick_hist[ticks]++;
    };

    auto starttime = std::chrono::high_resolution_clock::now();
    uint64_t message_count = 0;

    uint16_t aapl_locate = 0;
    auto last_snapshot = starttime;
    
    SPSCQueue<BookSnapshot, 1024> snapshot_queue;
    std::atomic<bool> engine_running{true};
        
    std::thread publisher_thread([&] (){
        
        BookSnapshot local_snap;
        while(engine_running.load(std::memory_order_relaxed)){
            if(snapshot_queue.pop(local_snap)){
                ssize_t written = sendto(sock, &local_snap, sizeof(BookSnapshot), 0, (struct sockaddr*)& addr, sizeof(addr));
                if(written < 0){
                    std::cerr << "UDP Send Failed\n";
                }
            }
            else std::this_thread::yield();
        }
    });

    while(ptr+2 <= end){
        
        uint16_t length = __builtin_bswap16(*reinterpret_cast<const uint16_t*>(ptr));
        ptr += 2; // exactly 2 bytes to correctly parse the binary

        if(length == 0 || ptr + length > end) break;

        const char* buffer = reinterpret_cast<const char*>(ptr);
        ptr += length;

        char msg_type = buffer[0];

        uint64_t t0 = 0;
        if(bench_mode) t0 = rdtsc_start();

        switch(msg_type){
            case 'A': {
                const AddOrder* msg = reinterpret_cast<const AddOrder*>(buffer);

                uint32_t price = __builtin_bswap32(msg -> price);
                uint32_t shares = __builtin_bswap32(msg -> shares);
                uint64_t order_ref = __builtin_bswap64(msg -> order_ref);
                uint16_t stock_locate = __builtin_bswap16(msg -> stock_locate); 

                if(measure_window) record_add_distance(books[stock_locate], price);
                books[stock_locate].add(price, shares, msg->side);
                orders[order_ref] = {price, shares, stock_locate, msg->side};

                break;
            }

            case 'F': {
                const AddOrderMPID* msg = reinterpret_cast<const AddOrderMPID*>(buffer);
                uint32_t price = __builtin_bswap32(msg->price);
                uint32_t shares = __builtin_bswap32(msg->shares);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                uint16_t stock_locate = __builtin_bswap16(msg->stock_locate);

                if(measure_window) record_add_distance(books[stock_locate], price);
                books[stock_locate].add(price, shares, msg->side);
                orders[order_ref] = {price, shares, stock_locate, msg->side};

                break;
            }

            case 'E':
            case 'C': {
                const OrderExecuted* msg = reinterpret_cast<const OrderExecuted*>(buffer);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                uint32_t executed_shares = __builtin_bswap32(msg->executed_shares);
                auto it = orders.find(order_ref);
                if(it != orders.end()){
                    OrderInfo& info = it->second;
                    uint32_t r = std::min(executed_shares, info.shares);
                    books[info.locate].remove(info.price, r, info.side);
                    info.shares -= r;
                    if(info.shares == 0) orders.erase(it);
                }
                break;
            }

            case 'X': {
                const OrderCancel* msg = reinterpret_cast<const OrderCancel*>(buffer);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                uint32_t cancelled_shares = __builtin_bswap32(msg->cancelled_shares);
                auto it = orders.find(order_ref);
                if(it != orders.end()){
                    OrderInfo& info = it->second;
                    uint32_t r = std::min(cancelled_shares, info.shares);
                    books[info.locate].remove(info.price, r, info.side);
                    info.shares -= r;
                    if(info.shares == 0) orders.erase(it);
                }
                break;
            }   

            case 'D': {
                const DeleteOrder* msg = reinterpret_cast<const DeleteOrder*>(buffer);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                auto it = orders.find(order_ref);
                if(it != orders.end()){
                    OrderInfo& info = it->second;
                    books[info.locate].remove(info.price, info.shares, info.side);
                    orders.erase(it);
                }

                break;
            }

            case 'U': {
                const OrderReplace* msg = reinterpret_cast<const OrderReplace*>(buffer);
                uint64_t old_ref = __builtin_bswap64(msg->original_order_ref);
                uint64_t new_ref = __builtin_bswap64(msg->new_order_ref);
                uint32_t price = __builtin_bswap32(msg->price);
                uint32_t shares = __builtin_bswap32(msg->shares);
                auto it = orders.find(old_ref);
                if(it != orders.end()){
                    // Copy out before erasing/inserting: orders[new_ref] below may
                    // rehash and invalidate both `it` and any reference into the map.
                    OrderInfo old = it->second;
                    books[old.locate].remove(old.price, old.shares, old.side);
                    orders.erase(it);
                    books[old.locate].add(price, shares, old.side);   // replace keeps side + book
                    orders[new_ref] = {price, shares, old.locate, old.side};
                }

                break;
            }

            case 'R': {
                const StockDirectory* msg = reinterpret_cast<const StockDirectory*>(buffer);
                uint16_t locate = __builtin_bswap16(msg -> stock_locate);
                
                std::string ticker(msg->stock, 8);
                ticker.erase(ticker.find_last_not_of(' ') + 1); // as it is padded

                locate_to_symbol[locate] = ticker;
                symbol_to_locate[ticker] = locate;
                break;
            }

        }

        if(bench_mode && message_count >= BENCH_WARMUP)
            bench.record(rdtsc_end() - t0);

        if(aapl_locate == 0){
            auto it = symbol_to_locate.find("AAPL");
            if(it != symbol_to_locate.end()) aapl_locate = it -> second;
        }
        
        if(message_count % 10000 == 0){
            auto now = std::chrono::high_resolution_clock::now();
            double elapsed = std::chrono::duration<double>(now - last_snapshot).count();
            if(elapsed >= 0.1){
                uint64_t ts = 0;
                for(int i = 0; i < 6; i++)
                    ts = (ts << 8) | (uint8_t)buffer[5+i];
                    
                BookSnapshot snap = {};
                books[aapl_locate].fill_snapshot(5, "AAPL", ts, snap);
                snapshot_queue.push(snap);
                last_snapshot = now;
            }
        }
            
        message_count++;
    }

    {
        uint64_t fp = 1469598103934665603ULL;
        for(uint32_t l = 0; l < 65536; l++){
            for(const auto& lvl : books[l].bids){
                fp ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; fp *= 1099511628211ULL;
            }
            for(const auto& lvl : books[l].asks){
                fp ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; fp *= 1099511628211ULL;
            }
        }
        std::cout << "Book fingerprint: " << fp << "\n";
    }

    if(measure_window){
        uint64_t denom = wh_total;
        auto pct_within = [&](uint32_t W)->double{
            uint64_t c = 0;
            for(uint32_t t = 0; t < W && t < HISTMAX; t++) c += tick_hist[t];
            return denom ? 100.0 * c / denom : 0.0;
        };
        std::cout << "\n=== add-price distance from mid (1 tick = 1 cent) ===\n";
        std::cout << "  adds measured : " << wh_total << "\n";
        std::cout << "  sub-penny     : " << wh_subpenny
                  << " (" << (denom ? 100.0*wh_subpenny/denom : 0.0) << "%)\n";
        std::cout << "  one-sided(no mid): " << wh_nomid << "\n";
        for(uint32_t W : {16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u})
            std::cout << "  within +/-" << W << " ticks ($" << W/100.0 << ") : "
                      << pct_within(W) << "%\n";
        std::cout << "  beyond +/-" << HISTMAX << " ticks : "
                  << (denom ? 100.0*wh_overflow/denom : 0.0) << "%\n";
    }

    std::cout << "Total symbols: " << symbol_to_locate.size() << "\n";
    std::cout << "AAPL locate: " << aapl_locate << "\n";

    // Print a few symbols to sanity check
    int printed = 0;
    for(uint16_t i = 0; i < 65536 && printed < 10; i++){
        if(!locate_to_symbol[i].empty()){
            std::cout << "  locate " << i << " = " << locate_to_symbol[i] << "\n";
            printed++;
        }
    }

    auto endtime = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(endtime - starttime).count();
    std::cout << "Processed " << message_count << " messages in " << seconds << "s\n";
    std::cout << "Throughput: " << (message_count / seconds) / 1e6 << " M msg/sec \n";
    std::cout << "Ingestion:  " << (file_size / seconds) / 1e6 << " MB/sec\n";

    if(bench_mode) bench.report();
    
    engine_running.store(false, std::memory_order_release);
    publisher_thread.join();
    
    munmap((void*)data, file_size);
    close(fd);
    close(sock);

    return 0;
}