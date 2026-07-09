#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

struct ByteSource {
    virtual const uint8_t* next(size_t& len) = 0;
    virtual ~ByteSource() = default;
};

struct MmapSource : ByteSource {
    const uint8_t* data;
    size_t size;
    bool done = false;
    MmapSource(const uint8_t* d, size_t s) : data(d), size(s) {}
    const uint8_t* next(size_t& len) override {
        if(done) return nullptr;
        done = true; len = size; return data;
    }
};

struct ChunkedSource : ByteSource {
    const uint8_t* data;
    size_t size, pos = 0, chunk;
    ChunkedSource(const uint8_t* d, size_t s, size_t c) : data(d), size(s), chunk(c) {}
    const uint8_t* next(size_t& len) override {
        if(pos >= size) return nullptr;
        len = std::min(chunk, size - pos);
        const uint8_t* r = data + pos;
        pos += len;
        return r;
    }
};

template<typename Handler>
inline void run_framed(ByteSource& src, Handler&& process){
    constexpr size_t CARRY_CAP = 128;
    uint8_t carry[CARRY_CAP];
    size_t  carry_len = 0;

    size_t chunk_len;
    const uint8_t* chunk;
    while((chunk = src.next(chunk_len)) != nullptr){
        const uint8_t* p = chunk;
        const uint8_t* e = chunk + chunk_len;

        if(carry_len > 0){
            if(carry_len < 2){
                size_t take = std::min((size_t)(2 - carry_len), (size_t)(e - p));
                std::memcpy(carry + carry_len, p, take);
                carry_len += take; p += take;
            }
            if(carry_len >= 2){
                uint16_t length = __builtin_bswap16(*reinterpret_cast<const uint16_t*>(carry));
                size_t need = 2 + (size_t)length;
                if(length == 0 || need > CARRY_CAP){
                    carry_len = 0;
                } else {
                    size_t take = std::min(need - carry_len, (size_t)(e - p));
                    std::memcpy(carry + carry_len, p, take);
                    carry_len += take; p += take;
                    if(carry_len == need){
                        process(reinterpret_cast<const char*>(carry + 2));
                        carry_len = 0;
                    }
                }
            }
        }

        while(p + 2 <= e){
            uint16_t length = __builtin_bswap16(*reinterpret_cast<const uint16_t*>(p));
            if(length == 0){ p = e; break; }
            if(p + 2 + (size_t)length > e) break;
            process(reinterpret_cast<const char*>(p + 2));
            p += 2 + (size_t)length;
        }

        if(p < e){
            carry_len = (size_t)(e - p);
            std::memcpy(carry, p, carry_len);
        }
    }
}