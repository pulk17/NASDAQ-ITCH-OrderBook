#include <algorithm>
#include "engine.hpp"

uint16_t Engine::process_message(const char* buffer){
    switch(buffer[0]){
        case 'A': {
            const AddOrder* msg = reinterpret_cast<const AddOrder*>(buffer);
            uint32_t price = __builtin_bswap32(msg->price);
            uint32_t shares = __builtin_bswap32(msg->shares);
            uint64_t order_ref = __builtin_bswap64(msg->order_ref);
            uint16_t locate = __builtin_bswap16(msg->stock_locate);
            books[locate].add(price, shares, msg->side);
            orders[order_ref] = {price, shares, locate, msg->side};
            return locate;
        }
        case 'F': {
            const AddOrderMPID* msg = reinterpret_cast<const AddOrderMPID*>(buffer);
            uint32_t price = __builtin_bswap32(msg->price);
            uint32_t shares = __builtin_bswap32(msg->shares);
            uint64_t order_ref = __builtin_bswap64(msg->order_ref);
            uint16_t locate = __builtin_bswap16(msg->stock_locate);
            books[locate].add(price, shares, msg->side);
            orders[order_ref] = {price, shares, locate, msg->side};
            return locate;
        }
        case 'E':
        case 'C': {
            const OrderExecuted* msg = reinterpret_cast<const OrderExecuted*>(buffer);
            uint64_t order_ref = __builtin_bswap64(msg->order_ref);
            uint32_t executed_shares = __builtin_bswap32(msg->executed_shares);
            auto it = orders.find(order_ref);
            if(it != orders.end()){
                OrderInfo& info = it->second;
                uint16_t locate = info.locate;
                uint32_t r = std::min(executed_shares, info.shares);
                books[locate].remove(info.price, r, info.side);
                info.shares -= r;
                if(info.shares == 0) orders.erase(it);
                return locate;
            }
            return 0;
        }
        case 'X': {
            const OrderCancel* msg = reinterpret_cast<const OrderCancel*>(buffer);
            uint64_t order_ref = __builtin_bswap64(msg->order_ref);
            uint32_t cancelled_shares = __builtin_bswap32(msg->cancelled_shares);
            auto it = orders.find(order_ref);
            if(it != orders.end()){
                OrderInfo& info = it->second;
                uint16_t locate = info.locate;
                uint32_t r = std::min(cancelled_shares, info.shares);
                books[locate].remove(info.price, r, info.side);
                info.shares -= r;
                if(info.shares == 0) orders.erase(it);
                return locate;
            }
            return 0;
        }
        case 'D': {
            const DeleteOrder* msg = reinterpret_cast<const DeleteOrder*>(buffer);
            uint64_t order_ref = __builtin_bswap64(msg->order_ref);
            auto it = orders.find(order_ref);
            if(it != orders.end()){
                OrderInfo& info = it->second;
                uint16_t locate = info.locate;
                books[locate].remove(info.price, info.shares, info.side);
                orders.erase(it);
                return locate;
            }
            return 0;
        }
        case 'U': {
            const OrderReplace* msg = reinterpret_cast<const OrderReplace*>(buffer);
            uint64_t old_ref = __builtin_bswap64(msg->original_order_ref);
            uint64_t new_ref = __builtin_bswap64(msg->new_order_ref);
            uint32_t price = __builtin_bswap32(msg->price);
            uint32_t shares = __builtin_bswap32(msg->shares);
            auto it = orders.find(old_ref);
            if(it != orders.end()){
                OrderInfo old = it->second;
                
                books[old.locate].remove(old.price, old.shares, old.side);
                orders.erase(it);
                books[old.locate].add(price, shares, old.side);
                orders[new_ref] = {price, shares, old.locate, old.side};
                return old.locate;
            }
            return 0;
        }
        case 'R': {
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