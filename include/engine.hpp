#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <memory>
#include <ankerl/unordered_dense.h>

#include "orderbook.hpp"
#include "messages.hpp"

struct Engine {
    std::unique_ptr<OrderBook[]> books;
    ankerl::unordered_dense::map<uint64_t, OrderInfo> orders;
    std::array<std::string, 65536> locate_to_symbol;
    ankerl::unordered_dense::map<std::string, uint16_t> symbol_to_locate;

    explicit Engine(size_t reserve_orders = 8000000) : books(std::make_unique<OrderBook[]>(65536)) {
        orders.max_load_factor(0.5);
        orders.reserve(reserve_orders);
    }

    uint16_t process_message(const char* buffer);
};