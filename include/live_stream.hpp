#pragma once
// --live: replay the tape at market pace (times --speed) and stream one
// symbol's top-5 book (60/s) plus a throughput frame (5/s) over UDP to
// tools/ws_relay.py, which forwards to report/live-native.html. The relay can
// send "SYM" back on the control port to switch symbols mid-run.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "engine.hpp"
#include "spsc_queue.hpp"

struct LiveStream {
    using clk = std::chrono::high_resolution_clock;
    #pragma pack(push,1)
    struct Stats { uint64_t ts_ns; uint64_t msgs; double mps; double progress; };
    #pragma pack(pop)

    static constexpr uint64_t START_NS = 33900000000000ULL;   // 09:25: skip premarket at full speed
    static constexpr uint64_t END_NS   = 57600000000000ULL;   // 16:00: stop before after-hours
    static constexpr uint16_t DATA_PORT = 12345, CTRL_PORT = 12346;

    std::string symbol;
    double   speed;                      // market-time multiplier, 0 = unpaced
    uint16_t locate = 0;
    bool     started = false;
    uint64_t ts_start = 0;
    double   slept_s = 0;                // pacing sleep, excluded from the reported rate
    clk::time_point run_start, wall_start, last_pub, last_stats;

    int sock = -1, ctrl_sock = -1;
    sockaddr_in addr{};
    SPSCQueue<BookSnapshot, 1024> queue;
    std::atomic<bool> running{true};
    std::thread publisher;

    LiveStream(std::string sym, double spd, clk::time_point start)
        : symbol(std::move(sym)), speed(spd), run_start(start) {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        addr.sin_family = AF_INET;
        addr.sin_port = htons(DATA_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in caddr{};
        caddr.sin_family = AF_INET; caddr.sin_port = htons(CTRL_PORT);
        caddr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if(bind(ctrl_sock, (sockaddr*)&caddr, sizeof caddr) < 0){
            std::cerr << "Warning: control port " << CTRL_PORT << " busy — symbol switching disabled\n";
            close(ctrl_sock); ctrl_sock = -1;
        } else fcntl(ctrl_sock, F_SETFL, O_NONBLOCK);

        // Socket writes happen off the engine thread, behind a lock-free queue.
        publisher = std::thread([this]{
            BookSnapshot snap;
            while(running.load(std::memory_order_relaxed)){
                if(queue.pop(snap)){
                    if(sendto(sock, &snap, sizeof snap, 0, (sockaddr*)&addr, sizeof addr) < 0)
                        std::cerr << "UDP Send Failed\n";
                }
                else std::this_thread::yield();
            }
        });
    }

    ~LiveStream(){
        running.store(false, std::memory_order_release);
        publisher.join();
        if(sock >= 0) close(sock);
        if(ctrl_sock >= 0) close(ctrl_sock);
    }

    void on_message(Engine& engine, uint64_t ts, uint64_t message_count){
        if(locate == 0){
            auto it = engine.symbol_to_locate.find(symbol);
            if(it != engine.symbol_to_locate.end()) locate = it->second;
        }
        // Only touch the wall clock every 256 messages: a clock read per message
        // would throttle the engine well below its true rate. 256 messages is a
        // sub-millisecond slice of market time, so pacing stays smooth.
        if(!locate || ts < START_NS || ts >= END_NS || (message_count & 255) != 0) return;

        poll_control(engine);
        auto now = clk::now();
        if(!started){ started = true; ts_start = ts; wall_start = last_pub = last_stats = now; }
        if(speed > 0){
            double target = double(ts - ts_start) / 1e9 / speed;
            double actual = std::chrono::duration<double>(now - wall_start).count();
            if(target > actual + 0.001){
                auto s0 = clk::now();
                std::this_thread::sleep_for(std::chrono::duration<double>(target - actual));
                now = clk::now();
                slept_s += std::chrono::duration<double>(now - s0).count();
            }
        }
        if(std::chrono::duration<double>(now - last_pub).count() >= 0.016){
            BookSnapshot snap = {};
            engine.books[locate].fill_snapshot(5, symbol, ts, snap);
            queue.push(snap);
            last_pub = now;
        }
        if(std::chrono::duration<double>(now - last_stats).count() >= 0.2){
            // the engine's true compute rate: wall time minus pacing sleep
            double compute_s = std::chrono::duration<double>(now - run_start).count() - slept_s;
            double prog = std::clamp((double(ts)/1e9 - 34200) / (57600 - 34200), 0.0, 1.0);
            Stats st{ ts, message_count, message_count / std::max(compute_s, 1e-9), prog };
            sendto(sock, &st, sizeof st, 0, (sockaddr*)&addr, sizeof addr);
            last_stats = now;
        }
    }

private:
    // The browser asks (via the relay) to switch symbol.
    void poll_control(const Engine& engine){
        if(ctrl_sock < 0) return;
        char req[16]; ssize_t r;
        while((r = recvfrom(ctrl_sock, req, sizeof(req)-1, 0, nullptr, nullptr)) > 0){
            req[r] = 0;
            while(r > 0 && (req[r-1] == '\n' || req[r-1] == ' ')) req[--r] = 0;
            auto it = engine.symbol_to_locate.find(req);
            if(it != engine.symbol_to_locate.end()){ locate = it->second; symbol = req; }
        }
    }
};
