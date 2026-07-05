#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
#include <chrono>
#include <x86intrin.h>

static inline uint64_t rdtsc_start(){
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}
static inline uint64_t rdtsc_end(){
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

struct LatencyBench {
    static constexpr uint32_t MAX_NS = 65536;
    std::vector<uint64_t> hist;
    uint64_t overflow = 0;
    uint64_t count = 0;
    uint64_t sum_ns = 0;
    uint64_t max_ns = 0;
    double   ns_per_cycle = 0.0;

    LatencyBench() : hist(MAX_NS, 0) {}

    void calibrate(){
        auto t0 = std::chrono::steady_clock::now();
        uint64_t c0 = __rdtsc();
        while(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)) { /* spin */ }
        uint64_t c1 = __rdtsc();
        auto t1 = std::chrono::steady_clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
        ns_per_cycle = ns / (double)(c1 - c0);
    }

    inline void record(uint64_t cycles){
        uint64_t ns = (uint64_t)(cycles * ns_per_cycle);
        count++;
        sum_ns += ns;
        if(ns > max_ns) max_ns = ns;
        if(ns < MAX_NS) hist[ns]++;
        else overflow++;
    }

    uint64_t percentile(double p) const {
        if(count == 0) return 0;
        uint64_t target = (uint64_t)(count * p);
        uint64_t cum = 0;
        for(uint32_t ns = 0; ns < MAX_NS; ns++){
            cum += hist[ns];
            if(cum >= target) return ns;
        }
        return max_ns;
    }

    void report() const {
        printf("\n=== Per-message dispatch latency  (n = %lu, warmup discarded) ===\n", count);
        printf("  mean   : %8.1f ns\n", count ? (double)sum_ns / count : 0.0);
        printf("  p50    : %8lu ns\n", percentile(0.50));
        printf("  p90    : %8lu ns\n", percentile(0.90));
        printf("  p99    : %8lu ns\n", percentile(0.99));
        printf("  p99.9  : %8lu ns\n", percentile(0.999));
        printf("  p99.99 : %8lu ns\n", percentile(0.9999));
        printf("  max    : %8lu ns\n", max_ns);
        if(overflow)
            printf("  note   : %lu samples exceeded %u ns (counted only in max)\n", overflow, MAX_NS);
    }
};