#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>

template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), "Capacity must be a power of 2");
    
private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    
    T buffer_[Capacity];
    
public:
    bool push(const T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & (Capacity - 1);
        
        if(next_head == tail_.load(std::memory_order_acquire)) return false;
        
        buffer_[current_head] = item;
        
        head_.store(next_head, std::memory_order_release);
        return true;
    }
    
    bool pop(T& item){
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        
        if(current_tail == head_.load(std::memory_order_acquire)) return false;
        
        item = buffer_[current_tail];
        
        tail_.store((current_tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }
};