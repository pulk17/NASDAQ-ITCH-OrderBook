#include <iostream>
#include <cstdint>
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

inline void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    pthread_t current_thread = pthread_self();
    int result = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    
    if(result != 0) std::cerr << "Warning: Failed to pin thread to core " << core_id << " (Do you have enough cores?)\n";
    else std::cout << "Successfully pinned thread " << current_thread << " to CPU core " << core_id << "\n";
}

int main(){
    
    pin_thread_to_core(1);
    
    int fd = open("/home/pulk1t/LOB/12302019.NASDAQ_ITCH50", O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    const uint8_t* data = (const uint8_t*) mmap (nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
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

    auto books = std::make_unique<OrderBook[]>(65536);  //transfer from stack to heap [65536 is the max size of uint16_t]

    ankerl::unordered_dense::map<uint64_t, uint16_t> ref_to_locate;
    ref_to_locate.reserve(8000000);

    auto starttime = std::chrono::high_resolution_clock::now();
    int message_count = 0;

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
        const char* buffer = reinterpret_cast<const char*>(ptr);
        ptr += length;

        char msg_type = buffer[0];

        switch(msg_type){
            case 'A': {
                const AddOrder* msg = reinterpret_cast<const AddOrder*>(buffer);

                uint32_t price = __builtin_bswap32(msg -> price);
                uint32_t shares = __builtin_bswap32(msg -> shares);
                uint64_t order_ref = __builtin_bswap64(msg -> order_ref);
                uint16_t stock_locate = __builtin_bswap16(msg -> stock_locate); 

                books[stock_locate].add_order(order_ref, price, shares, msg->side);
                ref_to_locate[order_ref] = stock_locate;

                break;
            }

            case 'F': {
                const AddOrderMPID* msg = reinterpret_cast<const AddOrderMPID*>(buffer);
                uint32_t price = __builtin_bswap32(msg->price);
                uint32_t shares = __builtin_bswap32(msg->shares);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                uint16_t stock_locate = __builtin_bswap16(msg->stock_locate);
                books[stock_locate].add_order(order_ref, price, shares, msg->side);
                ref_to_locate[order_ref] = stock_locate;

                break;
            }

            case 'E':
            case 'C': {
                const OrderExecuted* msg = reinterpret_cast<const OrderExecuted*>(buffer);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                uint32_t executed_shares = __builtin_bswap32(msg->executed_shares);
                auto it = ref_to_locate.find(order_ref);
                if(it != ref_to_locate.end()){
                    if(books[it->second].reduce_order(order_ref, executed_shares))
                        ref_to_locate.erase(it);
                }
                break;
            }

            case 'X': {
                const OrderCancel* msg = reinterpret_cast<const OrderCancel*>(buffer);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                uint32_t cancelled_shares = __builtin_bswap32(msg->cancelled_shares);
                auto it = ref_to_locate.find(order_ref);
                if(it != ref_to_locate.end()){
                    if(books[it->second].reduce_order(order_ref, cancelled_shares))
                        ref_to_locate.erase(it);
                }
                break;
            }   

            case 'D': {
                const DeleteOrder* msg = reinterpret_cast<const DeleteOrder*>(buffer);
                uint64_t order_ref = __builtin_bswap64(msg->order_ref);
                auto it = ref_to_locate.find(order_ref);
                if(it != ref_to_locate.end()){
                    books[it->second].delete_order(order_ref);
                    ref_to_locate.erase(it);
                }
                
                break;
            }

            case 'U': {
                const OrderReplace* msg = reinterpret_cast<const OrderReplace*>(buffer);
                uint64_t old_ref = __builtin_bswap64(msg->original_order_ref);
                uint64_t new_ref = __builtin_bswap64(msg->new_order_ref);
                uint32_t price = __builtin_bswap32(msg->price);
                uint32_t shares = __builtin_bswap32(msg->shares);
                auto it = ref_to_locate.find(old_ref);
                if(it != ref_to_locate.end()){
                    uint16_t stock_locate = it->second;
                    books[stock_locate].replace_order(old_ref, new_ref, price, shares);
                    ref_to_locate.erase(it);
                    ref_to_locate[new_ref] = stock_locate;
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
    
    engine_running.store(false, std::memory_order_release);
    publisher_thread.join();
    
    munmap((void*)data, file_size);
    close(fd);
    close(sock);

    return 0;
}