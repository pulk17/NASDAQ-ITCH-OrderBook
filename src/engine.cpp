#include <algorithm>
#include "engine.hpp"

void Engine::book_add(uint16_t locate, uint32_t price, uint32_t shares, char side){
    books[locate].add(price, shares, side);
    if(track_ofi) books[locate].update_ofi();
}

void Engine::book_remove(uint16_t locate, uint32_t price, uint32_t shares, char side){
    books[locate].remove(price, shares, side);
    if(track_ofi) books[locate].update_ofi();
}

// Execute, cancel and delete all take shares off a live order; the order
// leaves the map once nothing is left.
uint16_t Engine::reduce(uint64_t order_ref, uint32_t shares){
    auto it = orders.find(order_ref);
    if(it == orders.end()) return 0;
    OrderInfo& info = it->second;
    uint16_t locate = info.locate;
    uint32_t r = std::min(shares, info.shares);
    book_remove(locate, info.price, r, info.side);
    info.shares -= r;
    if(info.shares == 0) orders.erase(it);
    return locate;
}

uint16_t Engine::process_message(const char* buffer){
    switch(buffer[0]){
        case 'A':
        case 'F': {     // 'F' is 'A' plus an MPID suffix; only the shared prefix is read
            const AddOrder* msg = reinterpret_cast<const AddOrder*>(buffer);
            uint32_t price = __builtin_bswap32(msg->price);
            uint32_t shares = __builtin_bswap32(msg->shares);
            uint64_t order_ref = __builtin_bswap64(msg->order_ref);
            uint16_t locate = __builtin_bswap16(msg->stock_locate);
            book_add(locate, price, shares, msg->side);
            orders[order_ref] = {price, shares, locate, msg->side};
            return locate;
        }
        case 'E':
        case 'C': {     // 'C' is 'E' plus an execution price; only the shared prefix is read
            const OrderExecuted* msg = reinterpret_cast<const OrderExecuted*>(buffer);
            return reduce(__builtin_bswap64(msg->order_ref), __builtin_bswap32(msg->executed_shares));
        }
        case 'X': {
            const OrderCancel* msg = reinterpret_cast<const OrderCancel*>(buffer);
            return reduce(__builtin_bswap64(msg->order_ref), __builtin_bswap32(msg->cancelled_shares));
        }
        case 'D': {
            const DeleteOrder* msg = reinterpret_cast<const DeleteOrder*>(buffer);
            return reduce(__builtin_bswap64(msg->order_ref), UINT32_MAX);
        }
        case 'U': {     // replace = delete the old order, add the new one (loses queue priority)
            const OrderReplace* msg = reinterpret_cast<const OrderReplace*>(buffer);
            uint64_t old_ref = __builtin_bswap64(msg->original_order_ref);
            uint64_t new_ref = __builtin_bswap64(msg->new_order_ref);
            uint32_t price = __builtin_bswap32(msg->price);
            uint32_t shares = __builtin_bswap32(msg->shares);
            auto it = orders.find(old_ref);
            if(it == orders.end()) return 0;
            OrderInfo old = it->second;
            orders.erase(it);
            book_remove(old.locate, old.price, old.shares, old.side);
            book_add(old.locate, price, shares, old.side);
            orders[new_ref] = {price, shares, old.locate, old.side};
            return old.locate;
        }
        case 'R': {     // stock directory: locate <-> ticker
            const StockDirectory* msg = reinterpret_cast<const StockDirectory*>(buffer);
            uint16_t locate = __builtin_bswap16(msg->stock_locate);
            std::string ticker(msg->stock, 8);
            ticker.erase(ticker.find_last_not_of(' ') + 1);
            locate_to_symbol[locate] = ticker;
            symbol_to_locate[ticker] = locate;
            return 0;
        }
    }
    return 0;
}

uint64_t Engine::fingerprint(uint64_t h) const{
    for(uint32_t l = 0; l < 65536; l++){
        for(const auto& lvl : books[l].all_bids()){
            h ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; h *= 1099511628211ULL;
        }
        for(const auto& lvl : books[l].all_asks()){
            h ^= ((uint64_t)l << 40) ^ ((uint64_t)lvl.price << 8) ^ lvl.shares; h *= 1099511628211ULL;
        }
    }
    return h;
}
